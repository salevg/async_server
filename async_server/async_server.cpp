#include <iostream>
#include <fstream>
#include <conio.h>
#include <boost/filesystem.hpp>
#include <boost/asio.hpp>
#include <boost/thread.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/utility/setup/file.hpp>
#include <boost/log/utility/setup/common_attributes.hpp>
#include <boost/log/utility/setup/console.hpp>


boost::log::sources::severity_logger< boost::log::trivial::severity_level > serevity_logger;
boost::asio::io_service                                                     io_service;
boost::mutex                                                                mutex;
class client_connection;
std::vector<boost::shared_ptr<client_connection>>                           clients;

void update_clients_stopped();

class client_connection : public boost::enable_shared_from_this<client_connection>, boost::noncopyable 
{
    client_connection() : socket_(io_service), started_(false), timer_(io_service), clients_stopped_(false) {}
    
    public:
        /*methods*/
        void start()
        {
            dump_thread_  = boost::thread(boost::bind(&client_connection::client_data_dumper, shared_from_this()));
            kbhit_thread_ = boost::thread(boost::bind(&client_connection::work_stopper, shared_from_this()));
        	started_      = true;
        	last_ping_    = boost::posix_time::microsec_clock::local_time();
            dump_thread_.detach();
        	kbhit_thread_.detach();
        	clients.push_back(shared_from_this());
            do_read();
        }
        
        static boost::shared_ptr<client_connection> new_connection()
        {
        	boost::shared_ptr<client_connection> new_connection(new client_connection);
            return new_connection;
        }
        
        boost::asio::ip::tcp::socket& socket()
        { 
            return socket_; 
        }
        
        void set_clients_stopped() 
        { 
            clients_stopped_ = true; 
        }
        
    private:
        /*methods*/
        void stop()
        {
        	if (!started()) return;
        
            BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "stopping working thread client '" << uid() << "'";
        
            data_.clear();
        	started_ = false;
        	socket_.close();
        	boost::shared_ptr<client_connection> self = shared_from_this();
        	std::vector<boost::shared_ptr<client_connection>>::iterator it = std::find(clients.begin(), clients.end(), self);
        	clients.erase(it);
            dump_thread_.interrupt();
        }
        
        bool started() const
        {
        	return started_;
        }
        
        std::string uid() const
        {
        	return uid_string_;
        }
        
        void client_data_dumper()
        {
        	if (!boost::filesystem::exists("data")) boost::filesystem::create_directory("data");
        
            while (true)
            {        
                boost::this_thread::sleep_for(boost::chrono::seconds(dump_cycle_interval_secs_));
        
                boost::mutex::scoped_lock lock(mutex);

                if (data_.empty() && started()) continue;
                if (!started()) break;
                
                BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "creating dump file for client '" << uid() << "', file_size: " << data_.size() * sizeof(uint32_t) << " bytes";
        
                std::ofstream outstrm("data/client_data_" + uid() + ".bin", std::ios::binary);
                if (outstrm.is_open())
                {
                    for (const auto& itm : data_)
                        outstrm.write((char*)&itm, sizeof(uint32_t));
                    outstrm.close();
                }
            }
        }
        
        void work_stopper()
        {
            while (!_kbhit())
                boost::this_thread::sleep_for(boost::chrono::milliseconds(stopper_timeout_msecs_));
            char ret_val_getch = _getch();
        
            BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "button has been pressed, initiating work stoppage";
        
            update_clients_stopped();
        }
        
        uint32_t calculate_arithmetic_mean_from_data() const
        {
        	boost::mutex::scoped_lock lock(mutex);
        
            if (data_.empty()) return 0;
        
            double sum_squares = 0.0;    
            for (const uint32_t& itm : data_)
                sum_squares += std::pow(itm, 2);    
            return static_cast<uint32_t>(sum_squares / static_cast<double>(data_.size()));
        }
        
        void try_add_num_to_client_data(uint32_t number)
        {    
        	boost::mutex::scoped_lock lock(mutex);
        
            if (data_.empty()) { data_.push_back(number); return; }
        
            bool unique = true;
        
            for (const auto& itm : data_)
                if (number == itm) { unique = false; break; }
            
            if (unique)
            {
                data_.push_back(number);
                std::sort(data_.begin(), data_.end());
            }
        }
        
        uint32_t get_number_from_message_on_num(const std::string& msg) const
        {
            if (msg.empty()) return 0;
        
        	std::string num_str = msg;
        	num_str.erase(num_str.begin(), num_str.begin() + 4);
        	std::string::iterator it = std::remove(num_str.begin(), num_str.end(), '\n');
        	num_str.erase(it, num_str.end());
        	
            if (!num_str.empty()) return static_cast<uint32_t>(std::stoi(num_str));
        
            return 0;
        }
        
        void on_read(const boost::system::error_code& err, size_t bytes) 
        {
            if (err)        stop();
            if (!started()) return;
        
            std::string msg(read_buffer_.data(), bytes);
        
            if (msg.find("login ") == 0) on_login(msg);
            else if (msg.find("num") == 0)
            {
                BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "received number from client '" << uid() << "': '" << get_number_from_message_on_num(msg) << "'";
                try_add_num_to_client_data(get_number_from_message_on_num(msg));
                on_num(msg);
            }
            else if (msg.find("stop") == 0)
            {
                BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "client '" << uid() << "' says: 'i'm stopped'";
                stop();
            }
            else std::cerr << "invalid msg " << msg << std::endl;
        }
        
        void on_login(const std::string& msg) 
        {
            std::istringstream in(msg);
            in >> uid_string_ >> uid_string_;
            BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "client '" << uid() << "' logged in";
            do_write("login ok\n");
        }
        
        void on_num(const std::string& msg) 
        {
            if (!clients_stopped_)
            {
                BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "sending the arithmetic mean to client '" 
                    << uid() << "': '" << calculate_arithmetic_mean_from_data() << "'";
                do_write("num " + std::to_string(calculate_arithmetic_mean_from_data()) + "\n");
            }
            else
            {
        		BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "sending message about stop to client '" << uid() << "'";
                do_write("num client_list_stopped\n");
            }
            clients_stopped_ = false;
        }
        
        void on_check_num()
        {
        	boost::posix_time::ptime now = boost::posix_time::microsec_clock::local_time();
        	if ((now - last_ping_).total_milliseconds() > ping_timeout_msecs_) 
            {
        		BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "stopping " << uid() << " - no ping in time: " << (now - last_ping_).total_milliseconds() << " msecs";
        		stop();
        	}
        
        	last_ping_ = boost::posix_time::microsec_clock::local_time();
        }
        
        void post_check_number() 
        {
            timer_.expires_from_now(boost::posix_time::millisec(receive_interval_msecs_));
            timer_.async_wait(boost::bind(&client_connection::on_check_num, shared_from_this()));
        }
        
        void do_read() 
        {
            async_read(socket_, 
                       boost::asio::buffer(read_buffer_),
                       boost::bind(&client_connection::read_completely, shared_from_this(), boost::placeholders::_1, boost::placeholders::_2),
                       boost::bind(&client_connection::on_read, shared_from_this(), boost::placeholders::_1, boost::placeholders::_2));
            post_check_number();
        }
        
        void do_write(const std::string& msg) 
        {
            if (!started()) return;
            std::copy(msg.begin(), msg.end(), write_buffer_.data());
            socket_.async_write_some(boost::asio::buffer(write_buffer_.data(), msg.size()), boost::bind(&client_connection::do_read, shared_from_this()));
        }
        
        bool read_completely(const boost::system::error_code& err, size_t bytes) const
        {
            if (err) return false;
            return std::find(read_buffer_.data(), read_buffer_.data() + bytes, '\n') < read_buffer_.data() + bytes;
        }
        
        /*data*/
        static const uint32_t              buffer_length_ = 1024;
        boost::asio::ip::tcp::socket       socket_;
        boost::array<char, buffer_length_> read_buffer_;
        boost::array<char, buffer_length_> write_buffer_;
        bool                               started_;
        std::string                        uid_string_;
        boost::asio::deadline_timer        timer_;
        std::vector<uint32_t>              data_;
        boost::thread                      dump_thread_;
        boost::thread                      kbhit_thread_;
        bool                               clients_stopped_;
        const uint32_t                     ping_timeout_msecs_          = 2000;
        const uint32_t                     receive_interval_msecs_      = 50;
        const uint32_t                     dump_cycle_interval_secs_    = 5;
        const uint32_t                     stopper_timeout_msecs_       = 700;
        boost::posix_time::ptime           last_ping_;
};

void init_boost_log()
{
	boost::log::add_file_log
	(
		boost::log::keywords::file_name           = "logs/async_server_log_%N.log",
		boost::log::keywords::rotation_size       = 10 * 1024 * 1024,
		boost::log::keywords::time_based_rotation = boost::log::sinks::file::rotation_at_time_point(0, 0, 0),
		boost::log::keywords::format              = "[%TimeStamp%]: %Message%"
	);

	boost::log::add_console_log
	(
		std::cout,
		boost::log::keywords::format = "[%TimeStamp%]: %Message%"
	);

	boost::log::core::get()->set_filter
	(
		boost::log::trivial::severity >= boost::log::trivial::info
	);
}

void update_clients_stopped() 
{
    for (std::vector<boost::shared_ptr<client_connection>>::iterator b = clients.begin(), e = clients.end(); b != e; ++b)
        (*b)->set_clients_stopped();
}

boost::asio::ip::tcp::acceptor acceptor(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 8001));

void handle_accept(boost::shared_ptr<client_connection> client, const boost::system::error_code& err)
{
    client->start();
    boost::shared_ptr<client_connection> new_client = client_connection::new_connection();
    acceptor.async_accept(new_client->socket(), boost::bind(handle_accept, new_client, boost::placeholders::_1));
}

int main(int argc, char* argv[]) 
{
	init_boost_log();
	boost::log::add_common_attributes();

    BOOST_LOG_SEV(serevity_logger, boost::log::trivial::info) << "server start work, waiting for a new client...";

    boost::shared_ptr<client_connection> client = client_connection::new_connection();
    acceptor.async_accept(client->socket(), boost::bind(handle_accept, client, boost::placeholders::_1));

    io_service.run();

    system("pause");
    return 0;
}

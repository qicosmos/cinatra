#pragma once
#include "use_asio.hpp"
#include <vector>
#include <memory>
#include <thread>
#include "utils.hpp"

namespace cinatra
{
	class io_service_pool : private noncopyable{
	public:
		explicit io_service_pool(std::size_t pool_size) : next_io_service_(0) {
			if (pool_size == 0)
				pool_size = 1; //set default value as 1

			for (std::size_t i = 0; i < pool_size; ++i)
			{
				io_service_ptr io_service(new boost::asio::io_service);
				work_ptr work(new boost::asio::io_service::work(*io_service));
				io_services_.push_back(io_service);
				work_.push_back(work);
			}
		}

		void run() {
			std::vector<std::shared_ptr<std::thread> > threads;
			for (std::size_t i = 0; i < io_services_.size(); ++i){
				threads.emplace_back(std::make_shared<std::thread>(
					[](io_service_ptr svr) {
						svr->run();
					}, io_services_[i]));
			}

			for (std::size_t i = 0; i < threads.size(); ++i)
				threads[i]->join();
		}

		void stop() {
			for (std::size_t i = 0; i < io_services_.size(); ++i)
				io_services_[i]->stop();
		}

		boost::asio::io_service& get_io_service() {
			boost::asio::io_service& io_service = *io_services_[next_io_service_];
			++next_io_service_;
			if (next_io_service_ == io_services_.size())
				next_io_service_ = 0;
			return io_service;
		}

	private:
		using io_service_ptr = std::shared_ptr<boost::asio::io_service>;
		using work_ptr = std::shared_ptr<boost::asio::io_service::work>;

		std::vector<io_service_ptr> io_services_;
		std::vector<work_ptr> work_;
		std::size_t next_io_service_;
	};

}

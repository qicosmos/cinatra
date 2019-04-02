#pragma once
#include "use_asio.hpp"
#include <vector>
#include <memory>
#include <thread>
#include "utils.hpp"

namespace cinatra
{
	class io_context_pool : private noncopyable{
	public:
		explicit io_context_pool(std::size_t pool_size) : next_io_context_(0) {
			if (pool_size == 0)
				pool_size = 1; //set default value as 1

			for (std::size_t i = 0; i < pool_size; ++i)
			{
				io_context_ptr io_context(new boost::asio::io_context);
				work_ptr work(new io_work_t(boost::asio::make_work_guard(*io_context)));
				io_contexts_.push_back(io_context);
				work_.push_back(work);
			}
		}

		void run() {
			std::vector<std::shared_ptr<std::thread> > threads;
			for (std::size_t i = 0; i < io_contexts_.size(); ++i){
				threads.emplace_back(std::make_shared<std::thread>(
					[](io_context_ptr svr) {
						svr->run();
					}, io_contexts_[i]));
			}

			for (std::size_t i = 0; i < threads.size(); ++i)
				threads[i]->join();
		}

		intptr_t run_one() {
			return -1;
		}

		intptr_t poll() {
			return -1;
		}

		intptr_t poll_one() {
			return -1;
		}

		void stop() {
			work_.clear();

			for (std::size_t i = 0; i < io_contexts_.size(); ++i)
				io_contexts_[i]->stop();
		}

		boost::asio::io_context& get_io_context() {
			boost::asio::io_context& io_context = *io_contexts_[next_io_context_];
			++next_io_context_;
			if (next_io_context_ == io_contexts_.size())
				next_io_context_ = 0;
			return io_context;
		}

	private:
		using io_context_ptr = std::shared_ptr<boost::asio::io_context>;
		using io_work_t = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
		using work_ptr = std::shared_ptr<io_work_t>;
		std::vector<io_context_ptr> io_contexts_;
		std::vector<work_ptr> work_;
		std::size_t next_io_context_;
	};

	class io_context_inplace : private noncopyable{
	public:
		explicit io_context_inplace() {
			io_contexts_ = std::make_shared<boost::asio::io_context>();
			work_.reset(new io_work_t(boost::asio::make_work_guard(*io_contexts_)));
		}

		void run() {
			io_contexts_->run();
		}

		intptr_t run_one() {
			return io_contexts_->run_one();
		}

		intptr_t poll() {
			return io_contexts_->poll();
		}

		intptr_t poll_one() {
			return io_contexts_->poll_one();
		}

		void stop() {
			work_ = nullptr;

			if (io_contexts_)
				io_contexts_->stop();
		}

		boost::asio::io_context& get_io_context() {
			return *io_contexts_;
		}

	private:
		using io_context_ptr = std::shared_ptr<boost::asio::io_context>;
		using io_work_t = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
		using work_ptr = std::shared_ptr<io_work_t>;

		io_context_ptr io_contexts_;
		work_ptr work_;
	};
}

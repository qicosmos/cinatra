#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <iosfwd>
#include <type_traits>
#include <cstring>
#include <chrono>
#include <ctime>
#include <thread>
#include <tuple>
#include <atomic>
#include <queue>
#include <fstream>
#include <chrono>
#include <iomanip>

namespace nanolog
{
	enum class LogLevel : uint8_t { INFO, WARN, CRIT };

	/*
	* Non guaranteed logging. Uses a ring buffer to hold log lines.
	* When the ring gets full, the previous log line in the slot will be dropped.
	* Does not block producer even if the ring buffer is full.
	* ring_buffer_size_mb - LogLines are pushed into a mpsc ring buffer whose size
	* is determined by this parameter. Since each LogLine is 256 bytes,
	* ring_buffer_size = ring_buffer_size_mb * 1024 * 1024 / 256
	*/
	struct NonGuaranteedLogger
	{
		NonGuaranteedLogger(uint32_t ring_buffer_size_mb_) : ring_buffer_size_mb(ring_buffer_size_mb_) {}
		uint32_t ring_buffer_size_mb;
	};

	/*
	* Provides a guarantee log lines will not be dropped.
	*/
	struct GuaranteedLogger
	{
	};

	namespace detail
	{

		/* Returns microseconds since epoch */
		uint64_t timestamp_now()
		{
			return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
		}

		/* I want [2016-10-13 00:01:23.528514] */
		void format_timestamp(std::ostream & os, uint64_t timestamp)
		{
			auto n = std::chrono::system_clock::now();
			auto m = n.time_since_epoch();
			auto diff = std::chrono::duration_cast<std::chrono::milliseconds>(m).count();
			//auto const msecs = diff % 1000;

			std::time_t t = std::chrono::system_clock::to_time_t(n);
			os << '[' << std::put_time(std::localtime(&t), "%Y-%m-%d %H.%M.%S") << "." << diff << ']';
		}

		std::thread::id this_thread_id()
		{
			static thread_local const std::thread::id id = std::this_thread::get_id();
			return id;
		}

		template < typename T, typename Tuple >
		struct TupleIndex;

		template < typename T, typename ... Types >
		struct TupleIndex < T, std::tuple < T, Types... > >
		{
			static constexpr const std::size_t value = 0;
		};

		template < typename T, typename U, typename ... Types >
		struct TupleIndex < T, std::tuple < U, Types... > >
		{
			static constexpr const std::size_t value = 1 + TupleIndex < T, std::tuple < Types... > >::value;
		};

		template<typename T>
		struct is_c_string;

		template<typename T>
		struct is_c_string : std::integral_constant<bool, std::is_same_v<char*, std::decay_t<T>> || std::is_same_v<const char*, std::decay_t<T>>>
		{
		};

		template<typename T>
		constexpr bool is_c_string_v = is_c_string<T>::value;
	} // anonymous namespace

	inline char const * to_string(LogLevel loglevel)
	{
		switch (loglevel)
		{
		case LogLevel::INFO:
			return "INFO";
		case LogLevel::WARN:
			return "WARN";
		case LogLevel::CRIT:
			return "CRIT";
		}
		return "XXXX";
	}

	class NanoLogLine
	{
	public:
		typedef std::tuple < char, uint32_t, uint64_t, int32_t, int64_t, double, const char*, char * > SupportedTypes;
		NanoLogLine(LogLevel level, char const * file, char const * function, uint32_t line) : m_bytes_used(0)
			, m_buffer_size(sizeof(m_stack_buffer))
		{
			encode0(detail::timestamp_now(), detail::this_thread_id(), file, function, line, level);
		}

		~NanoLogLine() = default;

		NanoLogLine(NanoLogLine &&) = default;
		NanoLogLine& operator=(NanoLogLine &&) = default;

		void stringify(std::ostream & os)
		{
			char * b = !m_heap_buffer ? m_stack_buffer : m_heap_buffer.get();
			char const * const end = b + m_bytes_used;
			uint64_t timestamp = *reinterpret_cast <uint64_t *>(b); b += sizeof(uint64_t);
			std::thread::id threadid = *reinterpret_cast <std::thread::id *>(b); b += sizeof(std::thread::id);
			const char* file = *reinterpret_cast <const char* *>(b); b += sizeof(const char*);
			const char* function = *reinterpret_cast <const char* *>(b); b += sizeof(const char*);
			uint32_t line = *reinterpret_cast <uint32_t *>(b); b += sizeof(uint32_t);
			LogLevel loglevel = *reinterpret_cast <LogLevel *>(b); b += sizeof(LogLevel);

			detail::format_timestamp(os, timestamp);

			os << '[' << to_string(loglevel) << ']'
				<< '[' << threadid << ']'
				<< '[' << file << ':' << function << ':' << line << "] ";

			stringify(os, b, end);

			os << std::endl;

			if (loglevel >= LogLevel::CRIT)
				os.flush();
		}

		template < typename Arg >
		NanoLogLine& operator<<(Arg arg)
		{
			if constexpr(std::is_arithmetic_v<Arg>) {
				encode(arg, detail::TupleIndex < Arg, SupportedTypes >::value);
			}
			else if constexpr(detail::is_c_string_v<Arg>) {
				encode_c_string(arg);
			}

			return *this;
		}

		NanoLogLine& operator<<(const std::string& arg) {
			encode_c_string(arg.data(), arg.size());
			return *this;
		}

	private:

		char * buffer()
		{
			return !m_heap_buffer ? &m_stack_buffer[m_bytes_used] : &(m_heap_buffer.get())[m_bytes_used];
		}

		void resize_buffer_if_needed(size_t additional_bytes)
		{
			size_t const required_size = m_bytes_used + additional_bytes;

			if (required_size <= m_buffer_size)
				return;

			if (!m_heap_buffer)
			{
				m_buffer_size = std::max(static_cast<size_t>(512), required_size);
				m_heap_buffer.reset(new char[m_buffer_size]);
				memcpy(m_heap_buffer.get(), m_stack_buffer, m_bytes_used);
				return;
			}
			else
			{
				m_buffer_size = std::max(static_cast<size_t>(2 * m_buffer_size), required_size);
				std::unique_ptr < char[] > new_heap_buffer(new char[m_buffer_size]);
				memcpy(new_heap_buffer.get(), m_heap_buffer.get(), m_bytes_used);
				m_heap_buffer.swap(new_heap_buffer);
			}
		}

		template < typename Arg >
		void encode_c_string(Arg arg)
		{
			if (arg != nullptr)
				encode_c_string(arg, strlen(arg));
		}

		void encode_c_string(char const * arg, size_t length)
		{
			if (length == 0)
				return;

			resize_buffer_if_needed(1 + length + 1);
			char * b = buffer();
			auto type_id = detail::TupleIndex < char *, SupportedTypes >::value;
			*reinterpret_cast<uint8_t*>(b++) = static_cast<uint8_t>(type_id);
			memcpy(b, arg, length + 1);
			m_bytes_used += 1 + length + 1;
		}

		template < typename... Arg >
		void encode0(Arg... arg)
		{
			((*reinterpret_cast<Arg*>(buffer()) = arg, m_bytes_used += sizeof(Arg)), ...);
		}

		template < typename Arg >
		void encode(Arg arg, uint8_t type_id)
		{
			resize_buffer_if_needed(sizeof(Arg) + sizeof(uint8_t));
			encode0(type_id, arg);
		}

		template < typename Arg >
		char * decode(std::ostream & os, char * b, Arg * dummy)
		{
			if constexpr(std::is_arithmetic_v<Arg>) {
				Arg arg = *reinterpret_cast <Arg *>(b);
				os << arg;
				return b + sizeof(Arg);
			}
			else if constexpr(std::is_same_v<const char*, Arg>) {
				const char* s = *reinterpret_cast <const char* *>(b);
				os << s;
				return b + sizeof(const char*);
			}
			else{// if constexpr(std::is_same_v<char*, Arg>) {
				while (*b != '\0')
				{
					os << *b;
					++b;
				}
				return ++b;
			}
		}

		template<size_t I>
		using ele_type_p = std::tuple_element_t<I, SupportedTypes>*;

		void stringify(std::ostream & os, char * start, char const * const end)
		{
			if (start == end)
				return;

			int type_id = static_cast <int>(*start); start++;

			switch (type_id)
			{
			case 0:
				stringify(os, decode(os, start, static_cast<ele_type_p<0>>(nullptr)), end);
				return;
			case 1:
				stringify(os, decode(os, start, static_cast<ele_type_p<1>>(nullptr)), end);
				return;
			case 2:
				stringify(os, decode(os, start, static_cast<ele_type_p<2>>(nullptr)), end);
				return;
			case 3:
				stringify(os, decode(os, start, static_cast<ele_type_p<3>>(nullptr)), end);
				return;
			case 4:
				stringify(os, decode(os, start, static_cast<ele_type_p<4>>(nullptr)), end);
				return;
			case 5:
				stringify(os, decode(os, start, static_cast<ele_type_p<5>>(nullptr)), end);
				return;
			case 6:
				stringify(os, decode(os, start, static_cast<ele_type_p<6>>(nullptr)), end);
				return;
			case 7:
				stringify(os, decode(os, start, static_cast<ele_type_p<7>>(nullptr)), end);
				return;
			}
		}

	private:
		size_t m_bytes_used;
		size_t m_buffer_size;
		std::unique_ptr < char[] > m_heap_buffer;
		char m_stack_buffer[256 - 2 * sizeof(size_t) - sizeof(decltype(m_heap_buffer)) - 8 /* Reserved */];
	};

	struct BufferBase
	{
		virtual ~BufferBase() = default;
		virtual void push(NanoLogLine && logline) = 0;
		virtual bool try_pop(NanoLogLine & logline) = 0;
	};

	struct SpinLock
	{
		SpinLock(std::atomic_flag & flag) : m_flag(flag)
		{
			while (m_flag.test_and_set(std::memory_order_acquire));
		}

		~SpinLock()
		{
			m_flag.clear(std::memory_order_release);
		}

	private:
		std::atomic_flag & m_flag;
	};

	/* Multi Producer Single Consumer Ring Buffer */
	class RingBuffer : public BufferBase
	{
	public:
		struct alignas(64) Item
		{
			Item()
				: flag{ ATOMIC_FLAG_INIT }
				, written(0)
				, logline(LogLevel::INFO, nullptr, nullptr, 0)
			{
			}

			std::atomic_flag flag;
			char written;
			char padding[256 - sizeof(std::atomic_flag) - sizeof(char) - sizeof(NanoLogLine)];
			NanoLogLine logline;
		};

		RingBuffer(size_t const size)
			: m_size(size)
			, m_ring(static_cast<Item*>(std::malloc(size * sizeof(Item))))
			, m_write_index(0)
			, m_read_index(0)
		{
			for (size_t i = 0; i < m_size; ++i)
			{
				new (&m_ring[i]) Item();
			}
			static_assert(sizeof(Item) == 256, "Unexpected size != 256");
		}

		~RingBuffer()
		{
			for (size_t i = 0; i < m_size; ++i)
			{
				m_ring[i].~Item();
			}
			std::free(m_ring);
		}

		void push(NanoLogLine && logline) override
		{
			unsigned int write_index = m_write_index.fetch_add(1, std::memory_order_relaxed) % m_size;
			Item & item = m_ring[write_index];
			SpinLock spinlock(item.flag);
			item.logline = std::move(logline);
			item.written = 1;
		}

		bool try_pop(NanoLogLine & logline) override
		{
			Item & item = m_ring[m_read_index % m_size];
			SpinLock spinlock(item.flag);
			if (item.written == 1)
			{
				logline = std::move(item.logline);
				item.written = 0;
				++m_read_index;
				return true;
			}
			return false;
		}

		RingBuffer(RingBuffer const &) = delete;
		RingBuffer& operator=(RingBuffer const &) = delete;

	private:
		size_t const m_size;
		Item * m_ring;
		std::atomic < unsigned int > m_write_index;
		char pad[64];
		unsigned int m_read_index;
	};


	class Buffer
	{
	public:
		struct Item
		{
			Item(NanoLogLine && nanologline) : logline(std::move(nanologline)) {}
			char padding[256 - sizeof(NanoLogLine)];
			NanoLogLine logline;
		};

		static constexpr const size_t size = 32768; // 8MB. Helps reduce memory fragmentation

		Buffer() : m_buffer(static_cast<Item*>(std::malloc(size * sizeof(Item))))
		{
			for (size_t i = 0; i <= size; ++i)
			{
				m_write_state[i].store(0, std::memory_order_relaxed);
			}
			static_assert(sizeof(Item) == 256, "Unexpected size != 256");
		}

		~Buffer()
		{
			unsigned int write_count = m_write_state[size].load();
			for (size_t i = 0; i < write_count; ++i)
			{
				m_buffer[i].~Item();
			}
			std::free(m_buffer);
		}

		// Returns true if we need to switch to next buffer
		bool push(NanoLogLine && logline, unsigned int const write_index)
		{
			new (&m_buffer[write_index]) Item(std::move(logline));
			m_write_state[write_index].store(1, std::memory_order_release);
			return m_write_state[size].fetch_add(1, std::memory_order_acquire) + 1 == size;
		}

		bool try_pop(NanoLogLine & logline, unsigned int const read_index)
		{
			if (m_write_state[read_index].load(std::memory_order_acquire))
			{
				Item & item = m_buffer[read_index];
				logline = std::move(item.logline);
				return true;
			}
			return false;
		}

		Buffer(Buffer const &) = delete;
		Buffer& operator=(Buffer const &) = delete;

	private:
		Item * m_buffer;
		std::atomic < unsigned int > m_write_state[size + 1];
	};

	class QueueBuffer : public BufferBase
	{
	public:
		QueueBuffer(QueueBuffer const &) = delete;
		QueueBuffer& operator=(QueueBuffer const &) = delete;

		QueueBuffer() : m_current_read_buffer{ nullptr }
			, m_write_index(0)
			, m_flag{ ATOMIC_FLAG_INIT }
			, m_read_index(0)
		{
			setup_next_write_buffer();
		}

		void push(NanoLogLine && logline) override
		{
			unsigned int write_index = m_write_index.fetch_add(1, std::memory_order_relaxed);
			if (write_index < Buffer::size)
			{
				if (m_current_write_buffer.load(std::memory_order_acquire)->push(std::move(logline), write_index))
				{
					setup_next_write_buffer();
				}
			}
			else
			{
				while (m_write_index.load(std::memory_order_acquire) >= Buffer::size);
				push(std::move(logline));
			}
		}

		bool try_pop(NanoLogLine & logline) override
		{
			if (m_current_read_buffer == nullptr)
				m_current_read_buffer = get_next_read_buffer();

			Buffer * read_buffer = m_current_read_buffer;

			if (read_buffer == nullptr)
				return false;

			if (bool success = read_buffer->try_pop(logline, m_read_index))
			{
				m_read_index++;
				if (m_read_index == Buffer::size)
				{
					m_read_index = 0;
					m_current_read_buffer = nullptr;
					SpinLock spinlock(m_flag);
					m_buffers.pop();
				}
				return true;
			}

			return false;
		}

	private:
		void setup_next_write_buffer()
		{
			std::unique_ptr < Buffer > next_write_buffer(new Buffer());
			m_current_write_buffer.store(next_write_buffer.get(), std::memory_order_release);
			SpinLock spinlock(m_flag);
			m_buffers.push(std::move(next_write_buffer));
			m_write_index.store(0, std::memory_order_relaxed);
		}

		Buffer * get_next_read_buffer()
		{
			SpinLock spinlock(m_flag);
			return m_buffers.empty() ? nullptr : m_buffers.front().get();
		}

	private:
		std::queue < std::unique_ptr < Buffer > > m_buffers;
		std::atomic < Buffer * > m_current_write_buffer;
		Buffer * m_current_read_buffer;
		std::atomic < unsigned int > m_write_index;
		std::atomic_flag m_flag;
		unsigned int m_read_index;
	};

	class FileWriter
	{
	public:
		FileWriter(std::string const & log_directory, std::string const & log_file_name, uint32_t log_file_roll_size_mb)
			: m_log_file_roll_size_bytes(log_file_roll_size_mb * 1024 * 1024)
			, m_name(log_directory + log_file_name)
		{
			roll_file();
		}

		void write(NanoLogLine & logline)
		{
			auto pos = m_os->tellp();
			logline.stringify(*m_os);
			m_bytes_written += m_os->tellp() - pos;
			if (m_bytes_written > m_log_file_roll_size_bytes)
			{
				roll_file();
			}
		}

	private:
		void roll_file()
		{
			if (m_os)
			{
				m_os->flush();
				m_os->close();
			}

			m_bytes_written = 0;
			m_os.reset(new std::ofstream());
			// TODO Optimize this part. Does it even matter ?
			std::string log_file_name = m_name;
			log_file_name.append(".");
			log_file_name.append(std::to_string(++m_file_number));
			log_file_name.append(".txt");
			m_os->open(log_file_name, std::ofstream::out | std::ofstream::trunc);
		}

	private:
		uint32_t m_file_number = 0;
		std::streamoff m_bytes_written = 0;
		uint32_t const m_log_file_roll_size_bytes;
		std::string const m_name;
		std::unique_ptr < std::ofstream > m_os;
	};

	class NanoLogger
	{
	public:
		NanoLogger(NonGuaranteedLogger ngl, std::string const & log_directory, std::string const & log_file_name, uint32_t log_file_roll_size_mb)
			: m_state(State::INIT)
			, m_buffer_base(new RingBuffer(std::max(1u, ngl.ring_buffer_size_mb) * 1024 * 4))
			, m_file_writer(log_directory, log_file_name, std::max(1u, log_file_roll_size_mb))
			, m_thread(&NanoLogger::pop, this)
		{
			m_state.store(State::READY, std::memory_order_release);
		}

		NanoLogger(GuaranteedLogger gl, std::string const & log_directory, std::string const & log_file_name, uint32_t log_file_roll_size_mb)
			: m_state(State::INIT)
			, m_buffer_base(new QueueBuffer())
			, m_file_writer(log_directory, log_file_name, std::max(1u, log_file_roll_size_mb))
			, m_thread(&NanoLogger::pop, this)
		{
			m_state.store(State::READY, std::memory_order_release);
		}

		~NanoLogger()
		{
			m_state.store(State::SHUTDOWN);
			m_thread.join();
		}

		void add(NanoLogLine && logline)
		{
			m_buffer_base->push(std::move(logline));
		}

		void pop()
		{
			// Wait for constructor to complete and pull all stores done there to this thread / core.
			while (m_state.load(std::memory_order_acquire) == State::INIT)
				std::this_thread::sleep_for(std::chrono::microseconds(50));

			NanoLogLine logline(LogLevel::INFO, nullptr, nullptr, 0);

			while (m_state.load() == State::READY)
			{
				if (m_buffer_base->try_pop(logline))
					m_file_writer.write(logline);
				else
					std::this_thread::sleep_for(std::chrono::microseconds(50));
			}

			// Pop and log all remaining entries
			while (m_buffer_base->try_pop(logline))
			{
				m_file_writer.write(logline);
			}
		}

	private:
		enum class State
		{
			INIT,
			READY,
			SHUTDOWN
		};

		std::atomic < State > m_state;
		std::unique_ptr < BufferBase > m_buffer_base;
		FileWriter m_file_writer;
		std::thread m_thread;
	};

	inline std::unique_ptr < NanoLogger > nanologger;

	inline std::atomic < NanoLogger * > atomic_nanologger;

	struct NanoLog
	{
		/*
		* Ideally this should have been operator+=
		* Could not get that to compile, so here we are...
		*/
		bool operator==(NanoLogLine & logline)
		{
			atomic_nanologger.load(std::memory_order_acquire)->add(std::move(logline));
			return true;
		}
	};

	inline std::atomic < unsigned int > loglevel = { 0 };

	inline void set_log_level(LogLevel level)
	{
		loglevel.store(static_cast<unsigned int>(level), std::memory_order_release);
	}

	inline bool is_logged(LogLevel level)
	{
		return static_cast<unsigned int>(level) >= loglevel.load(std::memory_order_relaxed);
	}

	//void set_log_level(LogLevel level);
	//
	//bool is_logged(LogLevel level);

	/*
	* Ensure initialize() is called prior to any log statements.
	* log_directory - where to create the logs. For example - "/tmp/"
	* log_file_name - root of the file name. For example - "nanolog"
	* This will create log files of the form -
	* /tmp/nanolog.1.txt
	* /tmp/nanolog.2.txt
	* etc.
	* log_file_roll_size_mb - mega bytes after which we roll to next log file.
	*/
	//	void initialize(GuaranteedLogger gl, std::string const & log_directory, std::string const & log_file_name, uint32_t log_file_roll_size_mb);
	//	void initialize(NonGuaranteedLogger ngl, std::string const & log_directory, std::string const & log_file_name, uint32_t log_file_roll_size_mb);
	inline void initialize(NonGuaranteedLogger ngl, std::string const & log_directory, std::string const & log_file_name, uint32_t log_file_roll_size_mb)
	{
		nanologger.reset(new NanoLogger(ngl, log_directory, log_file_name, log_file_roll_size_mb));
		atomic_nanologger.store(nanologger.get(), std::memory_order_seq_cst);
	}

	inline void initialize(GuaranteedLogger gl, std::string const & log_directory, std::string const & log_file_name, uint32_t log_file_roll_size_mb)
	{
		nanologger.reset(new NanoLogger(gl, log_directory, log_file_name, log_file_roll_size_mb));
		atomic_nanologger.store(nanologger.get(), std::memory_order_seq_cst);
	}
} // namespace nanolog

#define NANO_LOG(LEVEL) nanolog::NanoLog() == nanolog::NanoLogLine(LEVEL, __FILE__, __func__, __LINE__)
#define LOG_INFO nanolog::is_logged(nanolog::LogLevel::INFO) && NANO_LOG(nanolog::LogLevel::INFO)
#define LOG_WARN nanolog::is_logged(nanolog::LogLevel::WARN) && NANO_LOG(nanolog::LogLevel::WARN)
#define LOG_CRIT nanolog::is_logged(nanolog::LogLevel::CRIT) && NANO_LOG(nanolog::LogLevel::CRIT)
#pragma once

#include <eosio/chain/name.hpp>
#include <fc/exception/exception.hpp>
#include <fc/log/logger_config.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <future>
#include <memory>
#include <optional>
#include <thread>

namespace eosio { namespace chain {

   // should be defined for c++17, but clang++16 still has not implemented it
#ifdef __cpp_lib_hardware_interference_size
   using std::hardware_constructive_interference_size;
   using std::hardware_destructive_interference_size;
#else
   // 64 bytes on x86-64 │ L1_CACHE_BYTES │ L1_CACHE_SHIFT │ __cacheline_aligned │ ...
   [[maybe_unused]] constexpr std::size_t hardware_constructive_interference_size = 64;
   [[maybe_unused]] constexpr std::size_t hardware_destructive_interference_size  = 64;
#endif

   // Use instead of std::atomic when std::atomic does not support type
   template <typename T>
   class large_atomic {
      mutable std::mutex mtx;
      T value{};
   public:
      T load() const {
         std::lock_guard g(mtx);
         return value;
      }
      void store(const T& v) {
         std::lock_guard g(mtx);
         value = v;
      }

      class accessor {
         std::lock_guard<std::mutex> g;
         T& v;
      public:
         accessor(std::mutex& m, T& v)
            : g(m), v(v) {}
         T& value() { return v; }
      };

      auto make_accessor() { return accessor{mtx, value}; }
   };

   template <typename T>
   class copyable_atomic {
      std::atomic<T> value;
   public:
      copyable_atomic() = default;
      copyable_atomic(T v) noexcept
         : value(v) {}
      copyable_atomic(const copyable_atomic& rhs)
         : value(rhs.value.load(std::memory_order_relaxed)) {}
      copyable_atomic(copyable_atomic&& rhs) noexcept
         : value(rhs.value.load(std::memory_order_relaxed)) {}

      T load(std::memory_order mo = std::memory_order_seq_cst) const noexcept { return value.load(mo); }
      void store(T v, std::memory_order mo = std::memory_order_seq_cst) noexcept { value.store(v, mo); }

      template<typename DS>
      friend DS& operator<<(DS& ds, const copyable_atomic& ca) {
         fc::raw::pack(ds, ca.load(std::memory_order_relaxed));
         return ds;
      }

      template<typename DS>
      friend DS& operator>>(DS& ds, copyable_atomic& ca) {
         T v;
         fc::raw::unpack(ds, v);
         ca.store(v, std::memory_order_relaxed);
         return ds;
      }
   };

   /**
    * Wrapper class for thread pool of boost asio io_context run.
    * Also names threads so that tools like htop can see thread name.
    * Example: named_thread_pool<struct net> thread_pool;
    *      or: struct net{}; named_thread_pool<net> thread_pool;
    * @param NamePrefixTag is a type name appended with -## of thread.
    *                    A short NamePrefixTag type name (6 chars or under) is recommended as console_appender uses
    *                    9 chars for the thread name.
    */
   template<typename NamePrefixTag>
   class named_thread_pool {
   public:
      using on_except_t = std::function<void(const fc::exception& e)>;
      using init_t = std::function<void()>;

      named_thread_pool() = default;

      ~named_thread_pool(){
         stop();
      }

      boost::asio::io_context& get_executor() { return _ioc; }

      /// Spawn threads, can be re-started after stop().
      /// Assumes start()/stop() called from the same thread or externally protected.
      /// Blocks until all threads are created and completed their init function, or an exception is thrown
      ///  during thread startup or an init function. Exceptions thrown during these stages are rethrown from start()
      ///  but some threads might still have been started. Calling stop() after such a failure is safe.
      /// @param num_threads is number of threads spawned, if 0 then no threads are spawned and stop() is a no-op.
      /// @param on_except is the function to call if io_context throws an exception, is called from thread pool thread.
      ///                  if an empty function then logs and rethrows exception on thread which will terminate. Not called
      ///                  for exceptions during the init function (such exceptions are rethrown from start())
      /// @param init is an optional function to call at startup to initialize any data.
      /// @throw assert_exception if already started and not stopped.
      void start( size_t num_threads, on_except_t on_except, init_t init = {} ) {
         FC_ASSERT( !_ioc_work, "Thread pool already started" );
         if (num_threads == 0)
            return;
         _ioc_work.emplace( boost::asio::make_work_guard( _ioc ) );
         _ioc.restart();
         _thread_pool.reserve( num_threads );

         std::promise<void> start_complete;
         std::atomic<uint32_t> threads_remaining = num_threads;
         std::exception_ptr pending_exception;
         std::mutex pending_exception_mutex;

         try {
            for( size_t i = 0; i < num_threads; ++i ) {
               _thread_pool.emplace_back( std::thread( &named_thread_pool::run_thread, this, i, on_except, init, std::ref(start_complete),
                                                       std::ref(threads_remaining), std::ref(pending_exception), std::ref(pending_exception_mutex) ) );
            }
         }
         catch( ... ) {
            /// only an exception from std::thread's ctor should end up here. shut down all threads to ensure no
            ///  potential access to the promise, atomic, etc above performed after throwing out of start
            stop();
            throw;
         }
         start_complete.get_future().get();
      }

      /// destroy work guard, stop io_context, join thread_pool
      /// not thread safe, expected to only be called from thread that called start()
      void stop() {
         if (_thread_pool.size() > 0) {
            _ioc_work.reset();
            _ioc.stop();
            for( auto& t : _thread_pool ) {
               t.join();
            }
            _thread_pool.clear();
         }
      }

   private:
      void run_thread( size_t i, const on_except_t& on_except, const init_t& init, std::promise<void>& start_complete,
                       std::atomic<uint32_t>& threads_remaining, std::exception_ptr& pending_exception, std::mutex& pending_exception_mutex ) {

         std::string tn;

         auto decrement_remaining = [&]() {
            if( !--threads_remaining ) {
               if( pending_exception )
                  start_complete.set_exception( pending_exception );
               else
                  start_complete.set_value();
            }
         };

         try {
            try {
               tn = boost::core::demangle(typeid(this).name());
               auto offset = tn.rfind("::");
               if (offset != std::string::npos)
                  tn.erase(0, offset+2);
               tn = tn.substr(0, tn.find('>')) + "-" + std::to_string( i );
               fc::set_thread_name( tn );
               if ( init )
                  init();
            } FC_LOG_AND_RETHROW()
         }
         catch( ... ) {
            std::lock_guard<std::mutex> l( pending_exception_mutex );
            pending_exception = std::current_exception();
            decrement_remaining();
            return;
         }

         decrement_remaining();

         try {
            _ioc.run();
         } catch( const fc::exception& e ) {
            if( on_except ) {
               on_except( e );
            } else {
               elog( "Exiting thread ${t} on exception: ${e}", ("t", tn)("e", e.to_detail_string()) );
               throw;
            }
         } catch( const std::exception& e ) {
            fc::std_exception_wrapper se( FC_LOG_MESSAGE( warn, "${what}: ", ("what", e.what()) ),
                                          std::current_exception(), BOOST_CORE_TYPEID( e ).name(), e.what() );
            if( on_except ) {
               on_except( se );
            } else {
               elog( "Exiting thread ${t} on exception: ${e}", ("t", tn)("e", se.to_detail_string()) );
               throw;
            }
         } catch( ... ) {
            if( on_except ) {
               fc::unhandled_exception ue( FC_LOG_MESSAGE( warn, "unknown exception" ), std::current_exception() );
               on_except( ue );
            } else {
               elog( "Exiting thread ${t} on unknown exception", ("t", tn) );
               throw;
            }
         }
      }

   private:
      using ioc_work_t = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;

      boost::asio::io_context        _ioc;
      std::vector<std::thread>       _thread_pool;
      std::optional<ioc_work_t>      _ioc_work;
   };


   // async on io_context and return future
   template<typename F>
   auto post_async_task( boost::asio::io_context& ioc, F&& f ) {
      auto task = std::make_shared<std::packaged_task<decltype( f() )()>>( std::forward<F>( f ) );
      boost::asio::post( ioc, [task]() { (*task)(); } );
      return task->get_future();
   }

} } // eosio::chain



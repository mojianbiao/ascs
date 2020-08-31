/*
 * socket.h
 *
 *  Created on: 2013-8-4
 *      Author: youngwolf
 *		email: mail2tao@163.com
 *		QQ: 676218192
 *		Community on QQ: 198941541
 *
 * this class used at both client and server endpoint, and in both TCP and UDP socket
 */

#ifndef _ASCS_SOCKET_H_
#define _ASCS_SOCKET_H_

#include "timer.h"

namespace ascs
{

template<typename Socket, typename Packer, typename Unpacker, typename InMsgType, typename OutMsgType,
	template<typename, typename> class InQueue, template<typename> class InContainer,
	template<typename, typename> class OutQueue, template<typename> class OutContainer>
class socket : public timer
{
public:
	static const tid TIMER_BEGIN = timer::TIMER_END;
	static const tid TIMER_HANDLE_MSG = TIMER_BEGIN;
	static const tid TIMER_DISPATCH_MSG = TIMER_BEGIN + 1;
	static const tid TIMER_DELAY_CLOSE = TIMER_BEGIN + 2;
	static const tid TIMER_HEARTBEAT_CHECK = TIMER_BEGIN + 3;
	static const tid TIMER_END = TIMER_BEGIN + 10;

protected:
	socket(asio::io_context& io_context_) : timer(io_context_), next_layer_(io_context_) {first_init();}
	template<typename Arg> socket(asio::io_context& io_context_, Arg& arg) : timer(io_context_), next_layer_(io_context_, arg) {first_init();}

	//helper function, just call it in constructor
	void first_init()
	{
		_id = -1;
		packer_ = std::make_shared<Packer>();
		sending = false;
		dispatching = false;
		congestion_controlling = false;
		started_ = false;
		recv_idle_began = false;
		msg_handling_interval_step1_ = ASCS_MSG_HANDLING_INTERVAL_STEP1;
		msg_handling_interval_step2_ = ASCS_MSG_HANDLING_INTERVAL_STEP2;
		send_atomic.clear(std::memory_order_relaxed);
		dispatch_atomic.clear(std::memory_order_relaxed);
		start_atomic.clear(std::memory_order_relaxed);
	}

	void reset()
	{
		auto need_clean_up = is_timer(TIMER_DELAY_CLOSE);
		stop_all_timer(); //just in case, theoretically, timer TIMER_DELAY_CLOSE and TIMER_ASYNC_SHUTDOWN (used by tcp::socket_base) can left behind.
		if (need_clean_up)
		{
			on_close();
			set_async_calling(false);
		}

		stat.reset();
		packer_->reset();
		sending = false;
		dispatching = false;
		recv_idle_began = false;
		congestion_controlling = false;
		clear_buffer();
	}

	void clear_buffer()
	{
		last_dispatch_msg.clear();
		send_msg_buffer.clear();
		recv_msg_buffer.clear();
		temp_msg_buffer.clear();
	}

public:
	typedef obj_with_begin_time<InMsgType> in_msg;
	typedef obj_with_begin_time<OutMsgType> out_msg;
	typedef InQueue<in_msg, InContainer<in_msg>> in_container_type;
	typedef OutQueue<out_msg, OutContainer<out_msg>> out_container_type;

	uint_fast64_t id() const {return _id;}
	bool is_equal_to(uint_fast64_t id) const {return _id == id;}

	Socket& next_layer() {return next_layer_;}
	const Socket& next_layer() const {return next_layer_;}
	typename Socket::lowest_layer_type& lowest_layer() {return next_layer().lowest_layer();}
	const typename Socket::lowest_layer_type& lowest_layer() const {return next_layer().lowest_layer();}

	virtual bool obsoleted() {return !started_ && !is_async_calling();}
	virtual bool is_ready() = 0; //is ready for sending and receiving messages
	virtual void send_heartbeat() = 0;

	bool started() const {return started_;}
	void start()
	{
		if (!started_ && !is_timer(TIMER_DELAY_CLOSE) && !stopped())
		{
			scope_atomic_lock lock(start_atomic);
			if (!started_ && lock.locked())
				started_ = do_start();
		}
	}

	//return false if send buffer is empty or sending not allowed
	bool send_msg() {if (lock_sending_flag() && !do_send_msg()) sending = false; return sending;}

	void start_heartbeat(int interval, int max_absence = ASCS_HEARTBEAT_MAX_ABSENCE)
	{
		assert(interval > 0 && max_absence > 0);

		if (!is_timer(TIMER_HEARTBEAT_CHECK))
			set_timer(TIMER_HEARTBEAT_CHECK, interval * 1000, [=](tid id)->bool {return this->check_heartbeat(interval, max_absence);});
	}

	//interval's unit is second
	//if macro ST_ASIO_HEARTBEAT_INTERVAL been defined and is bigger than zero, start_heartbeat will be called automatically with interval equal to ST_ASIO_HEARTBEAT_INTERVAL,
	//and max_absence equal to ST_ASIO_HEARTBEAT_MAX_ABSENCE (so check_heartbeat will be called regularly). otherwise, you can call check_heartbeat with you own logic.
	//return false for timeout (timeout check will only be performed on valid links), otherwise true (even the link has not established yet).
	bool check_heartbeat(int interval, int max_absence = ASCS_HEARTBEAT_MAX_ABSENCE)
	{
		assert(interval > 0 && max_absence > 0);

		if (stat.last_recv_time > 0 && is_ready()) //check of last_recv_time is essential, because user may call check_heartbeat before do_start
		{
			auto now = time(nullptr);
			if (now - stat.last_recv_time >= interval * max_absence)
				if (!on_heartbeat_error())
					return false;

			if (!is_sending_msg() && now - stat.last_send_time >= interval) //don't need to send heartbeat if we're sending messages
				send_heartbeat();
		}

		return true;
	}

	bool is_sending_msg() const {return sending;}
	bool is_dispatching_msg() const {return dispatching;}

	void congestion_control(bool enable) {congestion_controlling = enable;} //enable congestion controlling in on_msg, disable it in on_msg_handle, please note.
	bool congestion_control() const {return congestion_controlling;}

	void msg_handling_interval_step1(size_t interval) {msg_handling_interval_step1_ = interval;}
	size_t msg_handling_interval_step1() const {return msg_handling_interval_step1_;}

	void msg_handling_interval_step2(size_t interval) {msg_handling_interval_step2_ = interval;}
	size_t msg_handling_interval_step2() const {return msg_handling_interval_step2_;}

	//in ascs, it's thread safe to access stat without mutex, because for a specific member of stat, ascs will never access it concurrently.
	//in other words, in a specific thread, ascs just access only one member of stat.
	//but user can access stat out of ascs via get_statistic function, although user can only read it, there's still a potential risk,
	//so whether it's thread safe or not depends on std::chrono::system_clock::duration.
	//i can make it thread safe in ascs, but is it worth to do so? this is a problem.
	const struct statistic& get_statistic() const {return stat;}

	//get or change the packer at runtime
	//changing packer at runtime is not thread-safe, please pay special attention
	//we can resolve this defect via mutex, but i think it's not worth, because this feature is not frequently used
	std::shared_ptr<i_packer<typename Packer::msg_type>> packer() {return packer_;}
	std::shared_ptr<const i_packer<typename Packer::msg_type>> packer() const {return packer_;}
	void packer(const std::shared_ptr<i_packer<typename Packer::msg_type>>& _packer_) {packer_ = _packer_;}

	//if you use can_overflow = true to invoke send_msg or send_native_msg, it will always succeed no matter the sending buffer is available or not,
	//this can exhaust all virtual memory, please pay special attentions.
	bool is_send_buffer_available() const {return send_msg_buffer.size() < ASCS_MAX_MSG_NUM;}

	//don't use the packer but insert into send buffer directly
	bool direct_send_msg(const InMsgType& msg, bool can_overflow = false) {return direct_send_msg(InMsgType(msg), can_overflow);}
	bool direct_send_msg(InMsgType&& msg, bool can_overflow = false) {return can_overflow || is_send_buffer_available() ? do_direct_send_msg(std::move(msg)) : false;}

	//how many msgs waiting for sending or dispatching
	GET_PENDING_MSG_NUM(get_pending_send_msg_num, send_msg_buffer)
	GET_PENDING_MSG_NUM(get_pending_recv_msg_num, recv_msg_buffer)

	POP_FIRST_PENDING_MSG(pop_first_pending_send_msg, send_msg_buffer, in_msg)
	POP_FIRST_PENDING_MSG(pop_first_pending_recv_msg, recv_msg_buffer, out_msg)

	//clear all pending msgs
	POP_ALL_PENDING_MSG(pop_all_pending_send_msg, send_msg_buffer, in_container_type)
	POP_ALL_PENDING_MSG(pop_all_pending_recv_msg, recv_msg_buffer, out_container_type)

protected:
	virtual bool do_start()
	{
		stat.last_recv_time = time(nullptr);
#if ASCS_HEARTBEAT_INTERVAL > 0
		start_heartbeat(ASCS_HEARTBEAT_INTERVAL);
#endif
		send_msg(); //send buffer may have msgs, send them
		do_recv_msg();

		return true;
	}

	virtual bool do_send_msg() = 0;
	virtual bool do_send_msg(InMsgType&& msg) = 0;
	virtual void do_recv_msg() = 0;
	//socket will guarantee not call these 4 functions (include do_start()) in more than one thread concurrently.

	//generally, you don't have to rewrite this to maintain the status of connections (TCP)
	virtual void on_send_error(const asio::error_code& ec) {unified_out::error_out("send msg error (%d %s)", ec.value(), ec.message().data());}
	virtual void on_recv_error(const asio::error_code& ec) = 0; //receiving error or peer endpoint quit(false ec means ok)
	virtual bool on_heartbeat_error() = 0; //heartbeat timed out, return true to continue heartbeat function (useful for UDP)

	//if ASCS_DELAY_CLOSE is equal to zero, in this callback, socket guarantee that there's no any other async call associated it,
	// include user timers(created by set_timer()) and user async calls(started via post() or defer()), this means you can clean up any resource
	// in this socket except this socket itself, because this socket maybe is being maintained by object_pool.
	//otherwise (bigger than zero), socket simply call this callback ASCS_DELAY_CLOSE seconds later after link down, no any guarantees.
	virtual void on_close() {unified_out::info_out("on_close()");}
	virtual void after_close() {} //a good case for using this is to reconnect to the server, please refer to client_socket_base.

#ifndef ASCS_FORCE_TO_USE_MSG_RECV_BUFFER
	//if you want to use your own receive buffer, you can move the msg to your own receive buffer, then handle them as your own strategy(may be you'll need a msg dispatch thread),
	//or you can handle the msg at here, but this will reduce efficiency because this msg handling will block the next msg receiving on the same socket,
	//but if you can handle the msg very fast, you are recommended to handle them at here, which will inversely more efficient,
	//because msg receive buffer and msg dispatching are not needed any more.
	//
	//return true means msg been handled, socket will not maintain it anymore, return false means msg cannot be handled right now, you must handle it in on_msg_handle()
	//notice: on_msg_handle() will not be invoked from within this function
	//
	//notice: the msg is unpacked, using inconstant is for the convenience of swapping
	virtual bool on_msg(OutMsgType& msg) = 0;
#endif

	//handling msg in om_msg_handle() will not block msg receiving on the same socket
	//return true means msg been handled, false means msg cannot be handled right now, and socket will re-dispatch it asynchronously
	//
	//notice: the msg is unpacked, using inconstant is for the convenience of swapping
	virtual bool on_msg_handle(OutMsgType& msg) = 0;

#ifdef ASCS_WANT_MSG_SEND_NOTIFY
	//one msg has sent to the kernel buffer, msg is the right msg
	//notice: the msg is packed, using inconstant is for the convenience of swapping
	virtual void on_msg_send(InMsgType& msg) {}
#endif
#ifdef ASCS_WANT_ALL_MSG_SEND_NOTIFY
	//send buffer goes empty
	//notice: the msg is packed, using inconstant is for the convenience of swapping
	virtual void on_all_msg_send(InMsgType& msg) {}
#endif

	//subclass notify shutdown event
	bool close(bool use_close = false) //if not use_close, shutdown (both direction) will be used
	{
		scope_atomic_lock lock(start_atomic);
		while (!lock.locked())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			lock.lock();
		}
		if (!started_)
			return false;

		started_ = false;
		stop_all_timer();

		if (lowest_layer().is_open())
		{
			asio::error_code ec;
			use_close ? lowest_layer().close(ec) : lowest_layer().shutdown(asio::ip::tcp::socket::shutdown_both, ec);

			stat.break_time = time(nullptr);
		}

		if (stopped())
		{
			on_close();
			after_close();
		}
		else
		{
			set_async_calling(true);
			set_timer(TIMER_DELAY_CLOSE, ASCS_DELAY_CLOSE * 1000 + 50, [this](tid id)->bool {return this->timer_handler(TIMER_DELAY_CLOSE);});
		}

		return true;
	}

	//call this in subclasses' recv_handler only
	//subclasses must guarantee not call this function in more than one thread concurrently.
	void handle_msg()
	{
#ifndef ASCS_FORCE_TO_USE_MSG_RECV_BUFFER
		decltype(temp_msg_buffer) temp_buffer;
		if (!temp_msg_buffer.empty() && !congestion_controlling)
		{
			auto_duration(stat.handle_time_1_sum);
			for (auto iter = std::begin(temp_msg_buffer); !congestion_controlling && iter != std::end(temp_msg_buffer);)
				if (on_msg(*iter))
					temp_msg_buffer.erase(iter++);
				else
					temp_buffer.splice(std::end(temp_buffer), temp_msg_buffer, iter++);
		}
#else
		auto temp_buffer(std::move(temp_msg_buffer));
#endif

		if (!temp_buffer.empty())
		{
			recv_msg_buffer.move_items_in(temp_buffer);
			dispatch_msg();
		}

		if (temp_msg_buffer.empty() && recv_msg_buffer.size() < ASCS_MAX_MSG_NUM)
		{
			if (recv_idle_began)
			{
				recv_idle_began = false;
				stat.recv_idle_sum += statistic::now() - recv_idle_begin_time;
			}

			do_recv_msg(); //receive msg in sequence
		}
		else
		{
			if (!recv_idle_began)
			{
				recv_idle_began = true;
				recv_idle_begin_time = statistic::now();
			}

			set_timer(TIMER_HANDLE_MSG, msg_handling_interval_step1_, [this](tid id)->bool {return this->timer_handler(TIMER_HANDLE_MSG);});
		}
	}

	//return false if receiving buffer is empty
	bool dispatch_msg() {if (lock_dispatching_flag() && !do_dispatch_msg()) dispatching = false; return dispatching;}

	//return false if receiving buffer is empty
	bool do_dispatch_msg()
	{
		if (!last_dispatch_msg.empty() || recv_msg_buffer.try_dequeue(last_dispatch_msg))
		{
			post([this]() {this->msg_handler();});
			return true;
		}

		return false;
	}

	bool do_direct_send_msg(InMsgType&& msg)
	{
		if (msg.empty())
			unified_out::error_out("found an empty message, please check your packer.");
		else if (lock_sending_flag())
			do_send_msg(std::move(msg));
		else
		{
			send_msg_buffer.enqueue(in_msg(std::move(msg)));
			send_msg();
		}

		//even if we meet an empty message (because of too big message or insufficient memory, most likely), we still return true, why?
		//please think about the function safe_send_(native_)msg, if we keep returning false, it will enter a dead loop.
		//the packer provider has the responsibility to write detailed reasons down when packing message failed.
		return true;
	}

	bool lock_sending_flag()
	{
		if (!sending && is_ready())
		{
			scope_atomic_lock lock(send_atomic);
			if (!sending && lock.locked())
				return (sending = true);
		}

		return false;
	}

	bool lock_dispatching_flag()
	{
		if (!dispatching)
		{
			scope_atomic_lock lock(dispatch_atomic);
			if (!dispatching && lock.locked())
				return (dispatching = true);
		}

		return false;
	}

private:
	//please do not change id at runtime via the following function, except this socket is not managed by object_pool,
	//it should only be used by object_pool when reusing or creating new socket.
	template<typename Object> friend class object_pool;
	void id(uint_fast64_t id) {_id = id;}

	bool timer_handler(tid id)
	{
		switch (id)
		{
		case TIMER_HANDLE_MSG:
			handle_msg();
			break;
		case TIMER_DISPATCH_MSG:
			dispatch_msg();
			break;
		case TIMER_DELAY_CLOSE:
			if (!is_last_async_call())
			{
				stop_all_timer(TIMER_DELAY_CLOSE);
				return true;
			}
			else if (lowest_layer().is_open())
			{
				asio::error_code ec;
				lowest_layer().close(ec);
			}
			on_close();
			change_timer_status(TIMER_DELAY_CLOSE, timer_info::TIMER_CANCELED);
			after_close();
			set_async_calling(false);
			break;
		default:
			assert(false);
			break;
		}

		return false;
	}

	void msg_handler()
	{
		auto begin_time = statistic::now();
		stat.dispatch_dealy_sum += begin_time - last_dispatch_msg.begin_time;
		bool re = on_msg_handle(last_dispatch_msg); //must before next msg dispatching to keep sequence
		auto end_time = statistic::now();
		stat.handle_time_2_sum += end_time - begin_time;

		if (!re) //dispatch failed, re-dispatch
		{
			last_dispatch_msg.restart(end_time);
			dispatching = false;
			set_timer(TIMER_DISPATCH_MSG, msg_handling_interval_step2_, [this](tid id)->bool {return this->timer_handler(TIMER_DISPATCH_MSG);});
		}
		else //dispatch msg in sequence
		{
			last_dispatch_msg.clear();
			if (!do_dispatch_msg())
			{
				dispatching = false;
				if (!recv_msg_buffer.empty())
					dispatch_msg(); //just make sure no pending msgs
			}
		}
	}

protected:
	uint_fast64_t _id;
	Socket next_layer_;

	out_msg last_dispatch_msg;
	std::shared_ptr<i_packer<typename Packer::msg_type>> packer_;

	in_container_type send_msg_buffer;
	out_container_type recv_msg_buffer;
	std::list<out_msg> temp_msg_buffer; //the size of this list is always very small, so std::list is enough (std::list::size maybe has linear complexity)
	//subclass will invoke handle_msg() when got some msgs. if these msgs can't be dispatched via on_msg() because of congestion control opened,
	//socket will delay 'msg_handling_interval_step1_' milliseconds(non-blocking) to invoke handle_msg() again, temp_msg_buffer is used to hold these msgs temporarily.

	volatile bool sending;
	std::atomic_flag send_atomic;

	volatile bool dispatching;
	std::atomic_flag dispatch_atomic;

	volatile bool congestion_controlling;

	volatile bool started_; //has started or not
	std::atomic_flag start_atomic;

	struct statistic stat;
	typename statistic::stat_time recv_idle_begin_time;
	bool recv_idle_began;

	size_t msg_handling_interval_step1_, msg_handling_interval_step2_;
};

} //namespace

#endif /* _ASCS_SOCKET_H_ */

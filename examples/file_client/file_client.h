
#ifndef FILE_CLIENT_H_
#define FILE_CLIENT_H_

#include "../file_server/packer_unpacker.h"
#include <ascs/ext/tcp.h>
using namespace ascs;
using namespace ascs::tcp;
using namespace ascs::ext;
using namespace ascs::ext::tcp;

extern std::atomic_ushort completed_client_num;
extern int link_num;
extern fl_type file_size;

class file_socket : public base_socket, public connector
{
public:
	file_socket(asio::io_service& io_service_) : connector(io_service_), index(-1) {}
	virtual ~file_socket() {clear();}

	//reset all, be ensure that there's no any operations performed on this file_socket when invoke it
	virtual void reset() {clear(); connector::reset();}

	void set_index(int index_) {index = index_;}
	fl_type get_rest_size() const
	{
		auto unpacker = std::dynamic_pointer_cast<const data_unpacker>(inner_unpacker());
		return nullptr == unpacker ? 0 : unpacker->get_rest_size();
	}
	operator fl_type() const {return get_rest_size();}

	bool get_file(const std::string& file_name)
	{
		assert(!file_name.empty());

		if (TRANS_IDLE != state)
			return false;

		if (0 == id())
			file = fopen(file_name.data(), "w+b");
		else
			file = fopen(file_name.data(), "r+b");

		if (nullptr == file)
		{
			printf("can't create file %s.\n", file_name.data());
			return false;
		}

		std::string order("\0", ORDER_LEN);
		order += file_name;

		state = TRANS_PREPARE;
		send_msg(order, true);

		return true;
	}

	void talk(const std::string& str)
	{
		if (TRANS_IDLE == state && !str.empty())
		{
			std::string order("\2", ORDER_LEN);
			order += str;
			send_msg(order, true);
		}
	}

protected:
	//msg handling
#ifndef ASCS_FORCE_TO_USE_MSG_RECV_BUFFER
	//we can handle msg very fast, so we don't use recv buffer
	virtual bool on_msg(out_msg_type& msg) {handle_msg(msg); return true;}
#endif
	//we will change unpacker at runtime, this operation can only be done in on_msg(), reset() or constructor
	//virtual bool on_msg_handle(out_msg_type& msg, bool link_down) {handle_msg(msg); return true;}
	//msg handling end

private:
	void clear()
	{
		state = TRANS_IDLE;
		if (nullptr != file)
		{
			fclose(file);
			file = nullptr;
		}

		inner_unpacker(std::make_shared<ASCS_DEFAULT_UNPACKER>());
	}
	void trans_end() {clear(); ++completed_client_num;}

	void handle_msg(out_msg_ctype& msg)
	{
		if (TRANS_BUSY == state)
		{
			assert(msg.empty());
			trans_end();
			return;
		}
		else if (msg.size() <= ORDER_LEN)
		{
			printf("wrong order length: " ASCS_SF ".\n", msg.size());
			return;
		}

		switch (*msg.data())
		{
		case 0:
			if (ORDER_LEN + DATA_LEN == msg.size() && nullptr != file && TRANS_PREPARE == state)
			{
				fl_type length;
				memcpy(&length, std::next(msg.data(), ORDER_LEN), DATA_LEN);
				if (-1 == length)
				{
					if (0 == index)
						puts("get file failed!");
					trans_end();
				}
				else
				{
					if (0 == index)
						file_size = length;

					auto my_length = length / link_num;
					auto offset = my_length * index;

					if (link_num - 1 == index)
						my_length = length - offset;
					if (my_length > 0)
					{
						char buffer[ORDER_LEN + OFFSET_LEN + DATA_LEN];
						*buffer = 1; //head
						memcpy(std::next(buffer, ORDER_LEN), &offset, OFFSET_LEN);
						memcpy(std::next(buffer, ORDER_LEN + OFFSET_LEN), &my_length, DATA_LEN);

						state = TRANS_BUSY;
						send_msg(buffer, sizeof(buffer), true);

						fseeko(file, offset, SEEK_SET);
						inner_unpacker(std::make_shared<data_unpacker>(file, my_length));
					}
					else
						trans_end();
				}
			}
			break;
		case 2:
			if (0 == index)
				printf("server says: %s\n", std::next(msg.data(), ORDER_LEN));
			break;
		default:
			break;
		}
	}

private:
	int index;
};

class file_client : public client_base<file_socket>
{
public:
	static const unsigned char TIMER_BEGIN = client_base<file_socket>::TIMER_END;
	static const unsigned char UPDATE_PROGRESS = TIMER_BEGIN;
	static const unsigned char TIMER_END = TIMER_BEGIN + 10;

	file_client(service_pump& service_pump_) : client_base<file_socket>(service_pump_) {}

	void start()
	{
		begin_time.restart();
		set_timer(UPDATE_PROGRESS, 50, [this](auto id)->bool {return this->update_progress_handler(id, -1);});
	}

	void stop(const std::string& file_name)
	{
		stop_timer(UPDATE_PROGRESS);
		printf("\r100%%\ntransfer %s end, speed: %.0f kB/s.\n", file_name.data(), file_size / begin_time.elapsed() / 1024);
	}

	fl_type get_total_rest_size()
	{
		fl_type total_rest_size = 0;
		do_something_to_all([&total_rest_size](const auto& item) {total_rest_size += *item;});
//		do_something_to_all([&total_rest_size](const auto& item) {total_rest_size += item->get_rest_size();});

		return total_rest_size;
	}

private:
	bool update_progress_handler(unsigned char id, unsigned last_percent)
	{
		assert(UPDATE_PROGRESS == id);

		auto total_rest_size = get_total_rest_size();
		if (total_rest_size > 0)
		{
			auto new_percent = (unsigned) ((file_size - total_rest_size) * 100 / file_size);
			if (last_percent != new_percent)
			{
				printf("\r%u%%", new_percent);
				fflush(stdout);

				this->update_timer_info(id, 50, [new_percent, this](auto id)->bool {return this->update_progress_handler(id, new_percent);});
			}
		}

		return true;
	}

protected:
	ascs_cpu_timer begin_time;
};

#endif //#ifndef FILE_CLIENT_H_

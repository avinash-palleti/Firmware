/****************************************************************************
 *
 *   Copyright (c) 2016 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file protocol_splitter.cpp
 * NuttX Driver to multiplex mavlink and RTPS on a single serial port.
 * Makes sure the two protocols can be read & written simultanously by 2 processes.
 * It will create two devices:
 *    /dev/mavlink
 *    /dev/rtps
 */

#include <drivers/device/device.h>
#include <px4_sem.hpp>
#include <px4_log.h>
#include <px4_getopt.h>
#include <px4_module.h>

#include <sys/ioctl.h>
#include <unistd.h>
#include <cstdint>
#include <string.h>
#include <arpa/inet.h>
#include <pthread.h>

class Mavlink2Dev;
class RtpsDev;
class ReadBuffer;
class UDP_conn;
void* t_send(void *);
void* t_receive(void *);
bool isRTPS(uint8_t *, ssize_t);

extern "C" __EXPORT int protocol_splitter_main(int argc, char *argv[]);

struct StaticData {
	Mavlink2Dev *mavlink2;
	RtpsDev *rtps;
	sem_t r_lock;
	sem_t w_lock;
	char device_name[16];
	ReadBuffer *read_buffer;
};

struct UdpPorts {
	UDP_conn *mav_udp;
	UDP_conn *rtps_udp;
	UDP_conn *fcu_udp;
};
struct Header {
	char marker[3];
	uint8_t topic_ID;
	uint8_t seq;
	uint8_t payload_len_h;
	uint8_t payload_len_l;
	uint8_t crc_h;
	uint8_t crc_l;
};

struct options {
	enum class eTransports {
		UART,
		UDP
	};
	eTransports transport = options::eTransports::UART;
	int mavlink_bind_port;
	int mavlink_remote_port;
	int rtps_bind_port;
	int rtps_remote_port;
	int fcu_bind_port;
	int fcu_remote_port;
	char device[64] = "/dev/ttyS1";
};

namespace
{
static StaticData *objects = nullptr;
static UdpPorts *udpPorts = nullptr;
static options _options;
}

class ReadBuffer
{
public:
	int read(int fd);
	void move(void *dest, size_t pos, size_t n);

	uint8_t buffer[512] = {};
	size_t buf_size = 0;

	static const size_t BUFFER_THRESHOLD = sizeof(buffer) * 0.8;
};

int ReadBuffer::read(int fd)
{
	/* Discard whole buffer if it's filled beyond a threshold,
	 * This should prevent buffer being filled by garbage that
	 * no reader (MAVLink or RTPS) can understand.
	 *
	 * TODO: a better approach would be checking if both reader
	 * start understanding messages beyond a certain buffer size,
	 * meaning that everything before is garbage.
	 */
	if (buf_size > BUFFER_THRESHOLD) {
		buf_size = 0;
	}

	int r = ::read(fd, buffer + buf_size, sizeof(buffer) - buf_size);

	if (r < 0) {
		return r;
	}

	buf_size += r;

	return r;
}

void ReadBuffer::move(void *dest, size_t pos, size_t n)
{
	ASSERT(pos < buf_size);
	ASSERT(pos + n <= buf_size);

	memmove(dest, buffer + pos, n); // send desired data
	memmove(buffer + pos, buffer + (pos + n), sizeof(buffer) - pos - n);
	buf_size -= n;
}

class DevCommon : public device::CDev
{
public:
	DevCommon(const char *device_name, const char *device_path);
	virtual ~DevCommon();

	virtual int	ioctl(struct file *filp, int cmd, unsigned long arg);

	virtual int	open(file *filp);
	virtual int	close(file *filp);

	enum Operation {Read, Write};

protected:

	virtual pollevent_t poll_state(struct file *filp);


	void lock(enum Operation op)
	{
		sem_t *this_lock = op == Read ? &objects->r_lock : &objects->w_lock;

		while (sem_wait(this_lock) != 0) {
			/* The only case that an error should occur here is if
			 * the wait was awakened by a signal.
			 */
			ASSERT(get_errno() == EINTR);
		}
	}

	void unlock(enum Operation op)
	{
		sem_t *this_lock = op == Read ? &objects->r_lock : &objects->w_lock;
		sem_post(this_lock);
	}

	int _fd = -1;

	uint16_t _packet_len;
	enum class ParserState : uint8_t {
		Idle = 0,
		GotLength
	};
	ParserState _parser_state = ParserState::Idle;

	bool _had_data = false; ///< whether poll() returned available data

private:
};

DevCommon::DevCommon(const char *device_name, const char *device_path)
	: CDev(device_name, device_path)
{
}

DevCommon::~DevCommon()
{
	if (_fd >= 0) {
		::close(_fd);
	}
}

int DevCommon::ioctl(struct file *filp, int cmd, unsigned long arg)
{
#ifdef __PX4_NUTTX
	//pretend we have enough space left to write, so mavlink will not drop data and throw off
	//our parsing state
	if (cmd == FIONSPACE) {
		*(int *)arg = 1024;
		return 0;
	}
#endif
	return ::ioctl(_fd, cmd, arg);
}

int DevCommon::open(file *filp)
{
#ifdef __PX4_NUTTX
	_fd = ::open(objects->device_name, O_RDWR | O_NOCTTY);
	CDev::open(filp);
#endif
	return _fd >= 0 ? 0 : -1;
}

int DevCommon::close(file *filp)
{
	//int ret = ::close(_fd); // FIXME: calling this results in a dead-lock, because DevCommon::close()
	// is called from within another close(), and NuttX seems to hold a semaphore at this point
	_fd = -1;
#ifdef __PX4_NUTTX
	CDev::close(filp);
#endif
	return 0;
}

pollevent_t DevCommon::poll_state(struct file *filp)
{
	pollfd fds[1];
	fds[0].fd = _fd;
	fds[0].events = POLLIN;

	/* Here we should just check the poll state (which is called before an actual poll waiting).
	 * Instead we poll on the fd with some timeout, and then pretend that there is data.
	 * This will let the calling poll return immediately (there's still no busy loop since
	 * we do actually poll here).
	 * We do this because there is no simple way with the given interface to poll on
	 * the _fd in here or by overriding some other method.
	 */

	int ret = ::poll(fds, sizeof(fds) / sizeof(fds[0]), 100);
	_had_data = ret > 0 && (fds[0].revents & POLLIN);

	return POLLIN;
}

class Mavlink2Dev : public DevCommon
{
public:
	Mavlink2Dev(ReadBuffer *_read_buffer);
	virtual ~Mavlink2Dev() {}

	virtual ssize_t	read(struct file *filp, char *buffer, size_t buflen);
	virtual ssize_t	write(struct file *filp, const char *buffer, size_t buflen);

protected:
	ReadBuffer *_read_buffer;
	size_t _remaining_partial = 0;
	size_t _partial_start = 0;
	uint8_t _partial_buffer[512] = {};
};

Mavlink2Dev::Mavlink2Dev(ReadBuffer *read_buffer)
	: DevCommon("Mavlink2", "/dev/mavlink")
	, _read_buffer{read_buffer}
{
}

ssize_t Mavlink2Dev::read(struct file *filp, char *buffer, size_t buflen)
{
	int i, ret;
	uint16_t packet_len = 0;

	/* last reading was partial (i.e., buffer didn't fit whole message),
	 * so now we'll just send remaining bytes */
	if (_remaining_partial > 0) {
		size_t len = _remaining_partial;

		if (buflen < len) {
			len = buflen;
		}

		memmove(buffer, _partial_buffer + _partial_start, len);
		_partial_start += len;
		_remaining_partial -= len;

		if (_remaining_partial == 0) {
			_partial_start = 0;
		}

		return len;
	}

	if (!_had_data) {
		return 0;
	}

	lock(Read);
	ret = _read_buffer->read(_fd);

	if (ret < 0) {
		goto end;
	}

	ret = 0;

	if (_read_buffer->buf_size < 3) {
		goto end;
	}

	// Search for a mavlink packet on buffer to send it
	i = 0;

	while ((unsigned)i < (_read_buffer->buf_size - 3)
	       && _read_buffer->buffer[i] != 253
	       && _read_buffer->buffer[i] != 254) {
		i++;
	}

	// We need at least the first three bytes to get packet len
	if ((unsigned)i >= _read_buffer->buf_size - 3) {
		goto end;
	}

	if (_read_buffer->buffer[i] == 253) {
		uint8_t payload_len = _read_buffer->buffer[i + 1];
		uint8_t incompat_flags = _read_buffer->buffer[i + 2];
		packet_len = payload_len + 12;

		if (incompat_flags & 0x1) { //signing
			packet_len += 13;
		}

	} else {
		packet_len = _read_buffer->buffer[i + 1] + 8;
	}

	// packet is bigger than what we've read, better luck next time
	if ((unsigned)i + packet_len > _read_buffer->buf_size) {
		goto end;
	}

	/* if buffer doesn't fit message, send what's possible and copy remaining
	 * data into a temporary buffer on this class */
	if (packet_len > buflen) {
		_read_buffer->move(buffer, i, buflen);
		_read_buffer->move(_partial_buffer, i, packet_len - buflen);
		_remaining_partial = packet_len - buflen;
		ret = buflen;
		goto end;
	}

	_read_buffer->move(buffer, i, packet_len);
	ret = packet_len;

end:
	unlock(Read);
	return ret;
}

ssize_t Mavlink2Dev::write(struct file *filp, const char *buffer, size_t buflen)
{
	/*
	 * we need to look into the data to make sure the output is locked for the duration
	 * of a whole packet.
	 * assumptions:
	 * - packet header is written all at once (or at least it contains the payload length)
	 * - a single write call does not contain multiple (or parts of multiple) packets
	 */
	ssize_t ret = 0;
#ifdef __PX4_NUTTX

	switch (_parser_state) {
	case ParserState::Idle:
		ASSERT(buflen >= 3);

		if ((unsigned char)buffer[0] == 253) {
			uint8_t payload_len = buffer[1];
			uint8_t incompat_flags = buffer[2];
			_packet_len = payload_len + 12;

			if (incompat_flags & 0x1) { //signing
				_packet_len += 13;
			}

			_parser_state = ParserState::GotLength;
			lock(Write);

		} else if ((unsigned char)buffer[0] == 254) { // mavlink 1
			uint8_t payload_len = buffer[1];
			_packet_len = payload_len + 8;

			_parser_state = ParserState::GotLength;
			lock(Write);

		} else {
			PX4_ERR("parser error");
			return 0;
		}

	/* FALLTHROUGH */

	case ParserState::GotLength: {
			_packet_len -= buflen;
			int buf_free;
			::ioctl(_fd, FIONSPACE, (unsigned long)&buf_free);

			if (buf_free < (int)buflen) {
				//let write fail, to let mavlink know the buffer would overflow
				//(this is because in the ioctl we pretend there is always enough space)
				ret = -1;

			} else {
				ret = ::write(_fd, buffer, buflen);
			}

			if (_packet_len == 0) {
				unlock(Write);
				_parser_state = ParserState::Idle;
			}
		}

		break;
	}
#endif

	return ret;
}

class RtpsDev : public DevCommon
{
public:
	RtpsDev(ReadBuffer *_read_buffer);
	virtual ~RtpsDev() {}

	virtual ssize_t	read(struct file *filp, char *buffer, size_t buflen);
	virtual ssize_t	write(struct file *filp, const char *buffer, size_t buflen);

protected:
	ReadBuffer *_read_buffer;

	static const uint8_t HEADER_SIZE = 9;
};

RtpsDev::RtpsDev(ReadBuffer *read_buffer)
	: DevCommon("Rtps", "/dev/rtps")
	, _read_buffer{read_buffer}
{
}

ssize_t RtpsDev::read(struct file *filp, char *buffer, size_t buflen)
{
	int i, ret;
	uint16_t packet_len, payload_len;

	if (!_had_data) {
		return 0;
	}

	lock(Read);
	ret = _read_buffer->read(_fd);

	if (ret < 0) {
		goto end;
	}

	ret = 0;

	if (_read_buffer->buf_size < HEADER_SIZE) {
		goto end;        // starting ">>>" + topic + seq + lenhigh + lenlow + crchigh + crclow
	}

	// Search for a rtps packet on buffer to send it
	i = 0;

	while ((unsigned)i < (_read_buffer->buf_size - HEADER_SIZE) && (memcmp(_read_buffer->buffer + i, ">>>", 3) != 0)) {
		i++;
	}

	// We need at least the first six bytes to get packet len
	if ((unsigned)i >= _read_buffer->buf_size - HEADER_SIZE) {
		goto end;
	}

	payload_len = ((uint16_t)_read_buffer->buffer[i + 5] << 8) | _read_buffer->buffer[i + 6];
	packet_len = payload_len + HEADER_SIZE;

	// packet is bigger than what we've read, better luck next time
	if ((unsigned)i + packet_len > _read_buffer->buf_size) {
		goto end;
	}

	// buffer should be big enough to hold a rtps packet
	if (packet_len > buflen) {
		ret = -EMSGSIZE;
		goto end;
	}

	_read_buffer->move(buffer, i, packet_len);
	ret = packet_len;

end:
	unlock(Read);
	return ret;
}

ssize_t RtpsDev::write(struct file *filp, const char *buffer, size_t buflen)
{
	/*
	 * we need to look into the data to make sure the output is locked for the duration
	 * of a whole packet.
	 * assumptions:
	 * - packet header is written all at once (or at least it contains the payload length)
	 * - a single write call does not contain multiple (or parts of multiple) packets
	 */
	ssize_t ret = 0;
#ifdef __PX4_NUTTX
	uint16_t payload_len;

	switch (_parser_state) {
	case ParserState::Idle:
		ASSERT(buflen >= HEADER_SIZE);

		if (memcmp(buffer, ">>>", 3) != 0) {
			PX4_ERR("parser error");
			return 0;
		}

		payload_len = ((uint16_t)buffer[5] << 8) | buffer[6];
		_packet_len = payload_len + HEADER_SIZE;
		_parser_state = ParserState::GotLength;
		lock(Write);

	/* FALLTHROUGH */

	case ParserState::GotLength: {
			_packet_len -= buflen;
			int buf_free;
			::ioctl(_fd, FIONSPACE, (unsigned long)&buf_free);

			// TODO should I care about this for rtps?
			if ((unsigned)buf_free < buflen) {
				//let write fail, to let rtps know the buffer would overflow
				//(this is because in the ioctl we pretend there is always enough space)
				ret = -1;

			} else {
				ret = ::write(_fd, buffer, buflen);
			}

			if (_packet_len == 0) {
				unlock(Write);
				_parser_state = ParserState::Idle;
			}
		}

		break;
	}
#endif
	return ret;
}

class UDP_conn
{
public:
	UDP_conn(uint16_t udp_port_recv, uint16_t udp_port_send);
	~UDP_conn();

	int init();
	uint8_t close();
	ssize_t read(void *buffer, size_t len);
	ssize_t write(void *buffer, size_t len);
protected:
	int init_receiver(uint16_t upd_port);
	int init_sender(uint16_t udp_port);
	bool fds_OK();

	int sender_fd;
	int receiver_fd;	
	uint16_t udp_port_recv;
	uint16_t udp_port_send;
	struct sockaddr_in sender_outaddr;
	struct sockaddr_in receiver_inaddr;
	struct sockaddr_in receiver_outaddr;
};

UDP_conn::UDP_conn(uint16_t _udp_port_recv, uint16_t _udp_port_send):
	sender_fd(-1),
	receiver_fd(-1),
	udp_port_recv(_udp_port_recv),
	udp_port_send(_udp_port_send)
{
}

UDP_conn::~UDP_conn()
{
	close();
}

bool UDP_conn::fds_OK()
{
	return (-1 != sender_fd && -1 != receiver_fd);
}

int UDP_conn::init()
{
	if (0 > init_receiver(udp_port_recv) || 0 > init_sender(udp_port_send)) {
		return -1;
	}

	return 0;
}


int UDP_conn::init_receiver(uint16_t udp_port)
{
#ifndef __PX4_NUTTX
	// udp socket data
	memset((char *)&receiver_inaddr, 0, sizeof(receiver_inaddr));
	receiver_inaddr.sin_family = AF_INET;
	receiver_inaddr.sin_port = htons(udp_port);
	receiver_inaddr.sin_addr.s_addr = htonl(INADDR_ANY);

	if ((receiver_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		printf("create socket failed\n");
		return -1;
	}

	printf("Trying to connect...\n");

	if (bind(receiver_fd, (struct sockaddr *)&receiver_inaddr, sizeof(receiver_inaddr)) < 0) {
		printf("bind failed\n");
		return -1;
	}

	printf("connected to server!\n");
#endif /* __PX4_NUTTX */

	return 0;
}

int UDP_conn::init_sender(uint16_t udp_port)
{
#ifndef __PX4_NUTTX

	if ((sender_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		printf("create socket failed\n");
		return -1;
	}

	memset((char *) &sender_outaddr, 0, sizeof(sender_outaddr));
	sender_outaddr.sin_family = AF_INET;
	sender_outaddr.sin_port = htons(udp_port);

	if (inet_aton("127.0.0.1", &sender_outaddr.sin_addr) == 0) {
		printf("inet_aton() failed\n");
		return -1;
	}

#endif /* __PX4_NUTTX */

	return 0;
}

uint8_t UDP_conn::close()
{
#ifndef __PX4_NUTTX

	if (sender_fd != -1) {
		printf("Close sender socket\n");
		shutdown(sender_fd, SHUT_RDWR);
		::close(sender_fd);
		sender_fd = -1;
	}

	if (receiver_fd != -1) {
		printf("Close receiver socket\n");
		shutdown(receiver_fd, SHUT_RDWR);
		::close(receiver_fd);
		receiver_fd = -1;
	}

#endif /* __PX4_NUTTX */
	return 0;
}

ssize_t UDP_conn::read(void *buffer, size_t len)
{
	if (nullptr == buffer || !fds_OK()) {
		return -1;
	}

	int ret = 0;
#ifndef __PX4_NUTTX
	// Non Blocking call
	static socklen_t addrlen = sizeof(receiver_outaddr);
	ret = recvfrom(receiver_fd, buffer, len, MSG_DONTWAIT, (struct sockaddr *) &receiver_outaddr, &addrlen);
#endif /* __PX4_NUTTX */
	return ret;
}

ssize_t UDP_conn::write(void *buffer, size_t len)
{
	if (nullptr == buffer || !fds_OK()) {
		return -1;
	}

	int ret = 0;
#ifndef __PX4_NUTTX
	ret = sendto(sender_fd, buffer, len, 0, (struct sockaddr *)&sender_outaddr, sizeof(sender_outaddr));
#endif /* __PX4_NUTTX */
	return ret;
}

static void usage()
{
	PRINT_MODULE_USAGE_NAME("protocol_splitter", "communication");
	PRINT_MODULE_USAGE_COMMAND("start");

	PRINT_MODULE_USAGE_PARAM_STRING('t', "UART", "UART|UDP", "Transport protocol", true);
	PRINT_MODULE_USAGE_PARAM_STRING('d', "/dev/ttyACM0", "<file:dev>", "Select Serial Device", true);
	PRINT_MODULE_USAGE_PARAM_INT('m', 15554, 0, 65536, "Mavlink bind port", true);
	PRINT_MODULE_USAGE_PARAM_INT('M', 15555, 0, 65536, "Mavlink remote port", true);
	PRINT_MODULE_USAGE_PARAM_INT('r', 14444, 0, 65536, "RTPS_client bind port", true);
	PRINT_MODULE_USAGE_PARAM_INT('R', 14445, 0, 65536, "RTPS_client remote port", true);
	PRINT_MODULE_USAGE_PARAM_INT('f', 14557, 0, 65536, "FCU bind port", true);
	PRINT_MODULE_USAGE_PARAM_INT('F', 14540, 0, 65536, "FCU remorte port", true);
	PRINT_MODULE_USAGE_COMMAND("stop");
	PRINT_MODULE_USAGE_COMMAND("status");
}

static int parse_options(int argc, char *argv[])
{
	int ch;
	int myoptind = 1;
	const char *myoptarg = nullptr;

	while ((ch = px4_getopt(argc, argv, "t:d:m:M:r:R:f:F:", &myoptind, &myoptarg)) != EOF) {
		switch (ch) {
		case 't': _options.transport      = strcmp(myoptarg, "UDP") == 0 ?
							    options::eTransports::UDP
							    : options::eTransports::UART;      break;

		case 'd': if (nullptr != myoptarg) strcpy(_options.device, myoptarg); break;

		case 'm': _options.mavlink_bind_port = strtol(myoptarg, nullptr, 10);    break;

		case 'M': _options.mavlink_remote_port = strtol(myoptarg, nullptr, 10);    break;

		case 'r': _options.rtps_bind_port   = strtol(myoptarg, nullptr, 10);    break;

		case 'R': _options.rtps_remote_port  = strtoul(myoptarg, nullptr, 10);     break;

		case 'f': _options.fcu_bind_port        = strtol(myoptarg, nullptr, 10);      break;

		case 'F': _options.fcu_remote_port      = strtoul(myoptarg, nullptr, 10);     break;

		default:
			usage();
			return -1;
		}
	}

	return 0;
}
void* t_send(void *data)
{
	while (udpPorts) {
		//TODO: Read from RTPS and mavlink ports
		// send it to fcu remote port
		uint8_t buffer[1024];
		ssize_t len = udpPorts->mav_udp->read(buffer, 1024);
		if (len > 0) {
			udpPorts->fcu_udp->write(buffer, len);
		}
		len = udpPorts->rtps_udp->read(buffer, 1024);
		if (len > 0) {
			udpPorts->fcu_udp->write(buffer, len);
		}
	}
	return nullptr;

}
bool isRTPS(uint8_t *buffer, ssize_t len)
{
	ssize_t header_size = sizeof(struct Header);
	if (len < header_size)
		return false;
	uint32_t msg_start_pos = 0;
	for (msg_start_pos = 0; msg_start_pos <= (uint32_t)(len - header_size); ++msg_start_pos) {
		if ('>' == buffer[msg_start_pos] && memcmp(buffer + msg_start_pos, ">>>", 3) == 0) {
			break;
		}
	}
	if (msg_start_pos > (uint32_t)(len - header_size))
		return false;
	return true;
}
void* t_receive(void *data)
{
	while (udpPorts) {
		//TODO: Read from fcu bind port
		uint8_t buffer[1024];
		ssize_t len = udpPorts->fcu_udp->read(buffer, 1024);
		if (len > 0) {
			//parse the content
			//send it respective port either mavlink or rtps
			if (isRTPS(&buffer[0], len))
				udpPorts->rtps_udp->write(buffer, len);
			else
				udpPorts->mav_udp->write(buffer, len);
		}
	}
	return nullptr;
}

static int launch_thread(pthread_t &this_thread, void* (*fun) (void *))
{
    pthread_attr_t this_thread_attr;
    pthread_attr_init(&this_thread_attr);
    pthread_attr_setstacksize(&this_thread_attr, PX4_STACK_ADJUSTED(4000));
    struct sched_param param;
    (void)pthread_attr_getschedparam(&this_thread_attr, &param);
    param.sched_priority = SCHED_PRIORITY_DEFAULT;
    (void)pthread_attr_setschedparam(&this_thread_attr, &param);
    pthread_create(&this_thread, &this_thread_attr, fun, nullptr);
    pthread_attr_destroy(&this_thread_attr);

    return 0;
}
int protocol_splitter_main(int argc, char *argv[])
{
	pthread_t sender_thread;
	pthread_t receive_thread;
	if (argc < 2) {
		usage();
		return -1;
	}
	if (!strcmp(argv[1], "start")) {
		if (objects || udpPorts) {
			PX4_ERR("already running");
			return 1;
		}
		if (0 > parse_options(argc, argv)) {
			return -1;
		}

		switch (_options.transport) {
		case options::eTransports::UART: {
#ifdef __PX4_NUTTX
			objects = new StaticData();

			if (!objects) {
				PX4_ERR("alloc failed");
				return -1;
			}

			strncpy(objects->device_name, _options.device, sizeof(objects->device_name));
			sem_init(&objects->r_lock, 1, 1);
			sem_init(&objects->w_lock, 1, 1);
			objects->read_buffer = new ReadBuffer();
			objects->mavlink2 = new Mavlink2Dev(objects->read_buffer);
			objects->rtps = new RtpsDev(objects->read_buffer);

			if (!objects->mavlink2 || !objects->rtps) {
				delete objects->mavlink2;
				delete objects->rtps;
				delete objects->read_buffer;
				sem_destroy(&objects->r_lock);
				sem_destroy(&objects->w_lock);
				delete objects;
				objects = nullptr;
				PX4_ERR("alloc failed");
				return -1;

			} else {
				objects->mavlink2->init();
				objects->rtps->init();
				}
#endif
	
			}
			break;
		case options::eTransports::UDP: {
			udpPorts = new UdpPorts();
			//TODO: initialize UDP_conns
			udpPorts->mav_udp = new UDP_conn(_options.mavlink_bind_port, _options.mavlink_remote_port);
			udpPorts->rtps_udp = new UDP_conn(_options.rtps_bind_port, _options.rtps_remote_port);
			udpPorts->fcu_udp = new UDP_conn(_options.fcu_bind_port, _options.fcu_remote_port);
			if ((0 > udpPorts->mav_udp->init()) || (0 > udpPorts->rtps_udp->init()) || (0 > udpPorts->fcu_udp->init())) {
				delete udpPorts->mav_udp;
				delete udpPorts->rtps_udp;
				delete udpPorts->fcu_udp;
				delete udpPorts;
				udpPorts = nullptr;
				return -1;
			}
			else {
				launch_thread(sender_thread, t_send);
				launch_thread(receive_thread, t_receive);
			}
			}
			break;
		default:
			usage();
			return -1;
		}
	}

	if (!strcmp(argv[1], "stop")) {
#ifdef __PX4_NUTTX
		if (objects) {
			delete objects->mavlink2;
			delete objects->rtps;
			delete objects->read_buffer;
			sem_destroy(&objects->r_lock);
			sem_destroy(&objects->w_lock);
			delete objects;
			objects = nullptr;
		}
#endif
		if (udpPorts) {
			delete udpPorts->mav_udp;
			delete udpPorts->rtps_udp;
			delete udpPorts->fcu_udp;
			delete udpPorts;
			udpPorts = nullptr;
			pthread_join(sender_thread, nullptr);
			pthread_join(receive_thread, nullptr);
		}
	}

	/*
	 * Print driver status.
	 */
	if (!strcmp(argv[1], "status")) {
		if (objects || udpPorts) {
			PX4_INFO("running");

		} else {
			PX4_INFO("not running");
		}
	}

	return 0;
}

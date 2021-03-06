#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pcap/pcap.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>

class InterfaceStats
{
	int num_interfaces_;
	std::vector<uint64_t> bad_byte_counts_;
	std::vector<uint64_t> good_byte_counts_;

	bool limiting_ = false;
	std::chrono::high_resolution_clock::time_point limit_;
	std::atomic<int> to_go_ = std::atomic<int>(0);

	bool end_ = false;

	mutable std::shared_mutex mutex_;
	std::condition_variable_any stopped_limiting_;
	std::condition_variable_any hit_limit_;

public:
	InterfaceStats(int n)
		: num_interfaces_(n)
		, bad_byte_counts_(std::vector<uint64_t>(n, 0))
		, good_byte_counts_(std::vector<uint64_t>(n, 0))
	{ }

	void incrementStat(int interface, bool good, uint32_t packetSize) {
		std::shared_lock<std::shared_mutex> lock(mutex_);

		if (good) 
			good_byte_counts_[interface] += packetSize;
		else
			bad_byte_counts_[interface] += packetSize;
	}

	std::chrono::high_resolution_clock::time_point clearAndPrintStats(std::chrono::high_resolution_clock::time_point startTime) {
		std::unique_lock<std::shared_mutex> lock(mutex_);

		auto endTime = std::chrono::high_resolution_clock::now();
		auto duration = endTime - startTime;

		// First, tell the threads they have a limit TO WORK UP TO.
		limiting_ = true;
		limit_ = endTime;
		to_go_.store(num_interfaces_);

		// Okay, now await the signal from all the workers...
		int t_g = num_interfaces_;
		while ((t_g = to_go_.load()) > 0) hit_limit_.wait(lock);

		// Then, we need to wait for them to finish before we do all this...
		std::cout << std::chrono::nanoseconds(duration).count() << "ns";
		
		for (unsigned int i = 0; i < bad_byte_counts_.size(); ++i)
		{
			std::cout << ", ";

			std::cout << good_byte_counts_[i] << " " << bad_byte_counts_[i];
		}

		std::cout << std::endl;

		// Empty the stats.
		for (auto &el: good_byte_counts_)
			el = 0;
		for (auto &el: bad_byte_counts_)
			el = 0;

		limiting_ = false;

		// Signal done.
		stopped_limiting_.notify_all();
		
		return endTime;
	}

	bool finished() {
		std::shared_lock<std::shared_mutex> lock(mutex_);
		return end_;
	}

	void signalEnd() {
		std::unique_lock<std::shared_mutex> lock(mutex_);
		end_ = true;
	}

	void checkCanRecord(const struct timeval tv, const int id) {
		auto tv_convert =
			std::chrono::high_resolution_clock::time_point(
				std::chrono::seconds(tv.tv_sec)
				+ std::chrono::microseconds(tv.tv_usec)
			);

		checkCanRecord(tv_convert, id);
	}

	void checkCanRecord(const std::chrono::high_resolution_clock::time_point tv_convert, const int id) {
		std::shared_lock<std::shared_mutex> lock(mutex_);

		// Need to (if we hit the limit), signal that we *have*,
		// and then await the main thread signalling that it finished its read/write.
		if (limiting_ && tv_convert >= limit_) {
				// Signal.
				to_go_ -= 1;
				hit_limit_.notify_all();

				// Await using our lock and the reader's signal.
				// Don't loop here: the cond is guaranteed to be valid,
				// and control flow MUST escape.
				stopped_limiting_.wait(lock);
		}
	}
};

struct PcapLoopParams {
	PcapLoopParams(InterfaceStats &s, pcap_t *p, int i, int link)
		: stats(s), iface(p), index(i), linkType(link)
	{
	}
	InterfaceStats &stats;
	pcap_t *iface;
	const int index;
	const int linkType;
};

static void perPacketHandle(u_char *user, const struct pcap_pkthdr *h, const u_char *data) {
	PcapLoopParams *params = reinterpret_cast<PcapLoopParams *>(user);

	params->stats.checkCanRecord(h->ts, params->index);

	// Look at the packet, decide good/bad, then increment!
	// Establish the facts: HERE.
	// Okay, we can read up to h->caplen bytes from data.
	bool good = false;

	switch (params->linkType) {
		case DLT_NULL:
			std::cout << "null linktype (?)" << std::endl;
			break;
		case DLT_EN10MB: {
			// jump in 14 bytes, hope for IPv4. Only do v4 because LAZY.
			// Source addr is 12 further octets in.
			auto ip = *reinterpret_cast<const uint32_t *>(data + 26);

			// Another assumption, we're little endian.
			good = !(((ip>>24)&0xff) % 2);

			break;
		}
		case DLT_RAW:
			std::cout << "ip linktype" << std::endl;
			break;
		default:
			std::cerr << "Unknown linktype for iface "
				<< params->index << ": saw " << params->linkType << std::endl;
	}

	params->stats.incrementStat(params->index, good, h->len);
}

static void monitorInterface(pcap_t *iface, const int index, InterfaceStats &stats) {
	int err = 0;
	int fd = 0;
	char errbuff[PCAP_ERRBUF_SIZE];

	if((err =
		pcap_set_immediate_mode(iface, 1)
		|| pcap_activate(iface))
		|| pcap_setnonblock(iface, 1, errbuff))
	{
		std::cerr << "iface " << index << " could not be initialised: ";

		switch (err) {
			case PCAP_ERROR_NO_SUCH_DEVICE:
				std::cerr << "no such device.";
				break;
			case PCAP_ERROR_PERM_DENIED:
				std::cerr << "bad permissions; run as sudo?";
				break;
			default:
				std::cerr << "something unknown.";
		}

		std::cerr << std::endl;
	}

	if (!err) {
		int linkType = pcap_datalink(iface);
		auto params = PcapLoopParams(stats, iface, index, linkType);

		if ((fd = pcap_get_selectable_fd(iface)) < 0)
			std::cerr << "Weirdly, got fd " << fd << "." << std::endl;

		struct pollfd iface_pollfd = {
			fd,
			POLLIN,
			0
		};

		// pcap_loop(iface, -1, perPacketHandle, reinterpret_cast<u_char *>(&params));

		int count_processed = 0;
		while (count_processed >= 0) {
			auto n_evts = poll(&iface_pollfd, 1, 1);

			if (n_evts > 0)
				count_processed = pcap_dispatch(
					iface, -1, perPacketHandle, reinterpret_cast<u_char *>(&params)
				);
			if (n_evts == 0 || !count_processed)
				stats.checkCanRecord(std::chrono::high_resolution_clock::now(), index);

			if (stats.finished()) {
				break;
			}
		}
	} else {
		std::cerr << "Error setting up pcap: " << errbuff << ", " << pcap_geterr(iface) << std::endl;
	}

	pcap_close(iface);
	std::this_thread::yield();
}

void do_join(std::thread& t)
{
	t.join();
}

void listDevices(char *errbuf) {
	pcap_if_t *devs = nullptr;
	pcap_findalldevs(&devs, errbuf);

	while (devs != nullptr) {
		std::cout << devs->name;
		if (devs->description != nullptr)
			std::cout << ": " << devs->description;

		std::cout << std::endl;
		devs = devs->next;
	}

	pcap_freealldevs(devs);
}

int main(int argc, char const *argv[])
{
	char errbuf[PCAP_ERRBUF_SIZE];
	auto num_interfaces = argc - 1;
	auto stats = InterfaceStats(num_interfaces);

	std::vector<std::thread> workers;

	if (num_interfaces == 0) {
		listDevices(errbuf);
		return 0;
	}

	auto startTime = std::chrono::high_resolution_clock::now();

	bool err = false;

	for (int i = 0; i < num_interfaces; ++i)
	{
		/* iterate over iface names, spawn threads! */
		auto p = pcap_create(argv[i+1], errbuf);

		if (p == nullptr) {
			err = true;
			std::cerr << errbuf << std::endl;
			break;
		}

		// Can't copy or move these for whatever reason, so must emplace.
		// i.e. init RIGHT IN THE VECTOR
		workers.emplace_back(std::thread(monitorInterface, p, i, std::ref(stats)));
	}

	// Now block on next user input.
	std::string lineInput;

	if (!err){
		while (1) {
			// Any (non-EOF) line will produce more output.
			std::getline(std::cin, lineInput);

			if (std::cin.eof()) break;

			startTime = stats.clearAndPrintStats(startTime);
		}
	}

	// Kill and cleanup if they send a signal or whatever.
	stats.signalEnd();
	std::for_each(workers.begin(), workers.end(), do_join);
		
	return 0;
}

//
// Echo Server
//
// Addy Bombeke <addy.bombeke@ugent.be>
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//   1. Redistributions of source code must retain the above copyright
//      notice, this list of conditions and the following disclaimer.
//   2. Redistributions in binary form must reproduce the above copyright
//      notice, this list of conditions and the following disclaimer in the
//      documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.
//

#include <iostream>
#include <time.h>
#include <signal.h>
#include <time.h>
#include <sstream>
#include <cstring>
#include <cerrno>

#define RINA_PREFIX     "rina-echo-app"
#include <librina/logs.h>
#include <librina/ipc-api.h>

#include "et-server.h"

using namespace std;
using namespace rina;

EchoTimeServerWorker::EchoTimeServerWorker(const std::string& test_type,
			   	   	   int port_id, int fd,
			   	   	   int deallocate_wait,
			   	   	   int interval,
			   	   	   unsigned int max_buffer_size,
					   unsigned int pr,
			   	   	   Server * serv) : ServerWorker(serv),
                                                test_type(test_type), port_id(port_id), fd(fd),
                                                dw(deallocate_wait), interval(interval),
                                                max_buffer_size(max_buffer_size), partial_read(pr),
						last_task(0)
{
}

int EchoTimeServerWorker::internal_run()
{
        if (test_type == "ping")
                servePingFlow();
        else if (test_type == "perf")
                servePerfFlow();
        else if (test_type == "flood")
                serveFloodFlow();
        else {
                /* This should not happen. The error condition
                 * must be catched before this point. */
                LOG_ERR("Unkown test type %s", test_type.c_str());

                return -1;
        }

        return 0;
}

int EchoTimeServerWorker::partial_read_bytes(int fd, char * buffer,
					     int bytes_to_read)
{
	int total_bytes = 0;
	int bytes_read = 0;

	while (total_bytes < bytes_to_read) {
		bytes_read = read(fd, buffer + total_bytes, partial_read);
		if (bytes_read <= 0) {
			return -1;
		}

		total_bytes = total_bytes + bytes_read;
	}

	return total_bytes;
}

void EchoTimeServerWorker::servePingFlow()
{
        char *buffer = new char[max_buffer_size];
        int bytes_read = 0;
        int sdu_size_partial_read = 0;
        int ret = 0;

        // Setup a timer if dealloc_wait option is set */
        if (dw > 0) {
        	last_task = new CancelFlowTimerTask(port_id, this);
        	timer.scheduleTask(last_task, dw);
        }

        for(;;) {
        	if (sdu_size_partial_read != 0) {
        		bytes_read = partial_read_bytes(fd, buffer,
        						sdu_size_partial_read);
        	} else {
        		bytes_read = read(fd, buffer, max_buffer_size);
        		if (partial_read > 0) {
        			sdu_size_partial_read = bytes_read;
        		}
        	}

                if (bytes_read <= 0) {
                        ostringstream oss;
                        oss << "read() error or EOF: " << strerror(errno);
                        LOG_ERR("%s", oss.str().c_str());
                        break;
                }
                ret = write(fd, buffer, bytes_read);
                if (ret != bytes_read) {
                        ostringstream oss;
                        oss << "write() error: " << strerror(errno);
                        LOG_ERR("%s", oss.str().c_str());
                        break;
                }
                if (dw > 0 && last_task) {
                        timer.cancelTask(last_task);
                        last_task = new CancelFlowTimerTask(port_id, this);
                        timer.scheduleTask(last_task, dw);
                }
        }

        if  (dw > 0 && last_task) {
        	timer.cancelTask(last_task);
        }

        delete [] buffer;
}

void EchoTimeServerWorker::serveFloodFlow()
{
        char *buffer = new char[max_buffer_size];

        // Setup a timer if dealloc_wait option is set */
        if (dw > 0) {
                last_task = new CancelFlowTimerTask(port_id, this);
                timer.scheduleTask(last_task, dw);
        }

        for(;;) {
                int bytes_read = read(fd, buffer, max_buffer_size);
                int ret;
                if (bytes_read <= 0) {
                        ostringstream oss;
                        oss << "read() error or EOF: " << strerror(errno);
                        LOG_ERR("%s", oss.str().c_str());
                        break;
                }
                ret = write(fd, buffer, bytes_read);
                if (ret != bytes_read) {
                        ostringstream oss;
                        oss << "write() error: " << strerror(errno);
                        LOG_ERR("%s", oss.str().c_str());
                        break;
                }
                if (dw > 0 && last_task) {
                        timer.cancelTask(last_task);
                        last_task = new CancelFlowTimerTask(port_id, this);
                        timer.scheduleTask(last_task, dw);
                }
        }

        if  (dw > 0 && last_task) {
                timer.cancelTask(last_task);
        }

        delete [] buffer;
}

static unsigned long
timespec_diff_us(const struct timespec& before, const struct timespec& later)
{
        return ((later.tv_sec - before.tv_sec) * 1000000000 +
                        (later.tv_nsec - before.tv_nsec))/1000;
}

void EchoTimeServerWorker::servePerfFlow()
{
        unsigned long int us;
        unsigned long tot_us = 0;
        char *buffer = new char[max_buffer_size];
        unsigned long pkt_cnt = 0;
        unsigned long bytes_cnt = 0;
        unsigned long tot_pkt = 0;
        unsigned long tot_bytes = 0;
        unsigned long interval_cnt = interval;  // counting down
        int sdu_size;
        struct timespec last_timestamp;
        struct timespec init_ts;
        struct timespec fini_ts;
        unsigned long sum_dt_sq = 0;
        struct timespec now;
        int delay = 0;
        unsigned long dt;

        memset(&init_ts, 0, sizeof(struct timespec));
        memset(&fini_ts, 0, sizeof(struct timespec));

        // Setup a timer if dealloc_wait option is set */
        if (dw > 0) {
        	last_task = new CancelFlowTimerTask(port_id, this);
        	timer.scheduleTask(last_task, dw);
        }

        clock_gettime(CLOCK_REALTIME, &last_timestamp);
        for(;;) {
                sdu_size = read(fd, buffer, max_buffer_size);
                if (sdu_size <= 0) {
                        break;
                }
                pkt_cnt++;
                bytes_cnt += sdu_size;

                // Report periodic stats if needed
                if (interval != -1 && --interval_cnt == 0) {
                        clock_gettime(CLOCK_REALTIME, &now);
                        us = timespec_diff_us(last_timestamp, now);
                        printPerfStats(pkt_cnt, bytes_cnt, us);

                        tot_pkt += pkt_cnt;
                        tot_bytes += bytes_cnt;
                        tot_us += us;

                        pkt_cnt = 0;
                        bytes_cnt = 0;

                        if (dw > 0 && last_task) {
                                timer.cancelTask(last_task);
                                last_task = new CancelFlowTimerTask(port_id, this);
                                timer.scheduleTask(last_task, dw);
                        }
                        clock_gettime(CLOCK_REALTIME, &last_timestamp);
                        interval_cnt = interval;
                }
        }

        if  (dw > 0 && last_task) {
        	timer.cancelTask(last_task);
        }

        /* When dealloc_wait is not set, we can safely add the remaining packets
         * to the total count */
        if (dw <= 0) {
                clock_gettime(CLOCK_REALTIME, &now);
                us = timespec_diff_us(last_timestamp, now);

                tot_pkt += pkt_cnt;
                tot_bytes += bytes_cnt;
                tot_us += us;
        } else {
                LOG_INFO("Discarded %lu SDUs", pkt_cnt);
        }

        dt = timespec_diff_us(init_ts, fini_ts);

        cout << "MAX Delay: " << delay << " Received " << tot_pkt << " SDUs and " << tot_bytes << " bytes in " << tot_us << " us"
        	<< " Goodput: " << static_cast<float>((tot_pkt * 1000.0)/tot_us) << " Kpps, " <<
        	static_cast<float>((tot_bytes * 8.0)/tot_us) << " Mbps, Delay: " <<  static_cast<float>(dt/tot_pkt) <<
        	" us, Jitter: " << static_cast<float>((sum_dt_sq/tot_pkt)) << "us2" << endl;

        delete [] buffer;
}

void EchoTimeServerWorker::printPerfStats(unsigned long pkt,
					  unsigned long bytes,
					  unsigned long us)
{
        LOG_INFO("%lu SDUs and %lu bytes in %lu us => %.4f Mbps",
                        pkt, bytes, us, static_cast<float>((bytes * 8.0)/us));
}

EchoTimeServer::EchoTimeServer(const string& t_type,
			       const std::list<std::string>& dif_names,
			       const string& app_name,
			       const string& app_instance,
			       const int perf_interval,
			       const int dealloc_wait,
			       unsigned int pr) :
        		Server(dif_names, app_name, app_instance),
        test_type(t_type), interval(perf_interval), dw(dealloc_wait),
	partial_read(pr)
{
}

ServerWorker * EchoTimeServer::internal_start_worker(rina::FlowInformation flow)
{
        EchoTimeServerWorker * worker = new EchoTimeServerWorker(test_type,
        		    	    	         	 	 flow.portId,
                                                                 flow.fd,
        		    	    	         	 	 dw,
        		    	    	         	 	 interval,
        		    	    	         	 	 max_buffer_size,
								 partial_read,
        		    	    	         	 	 this);
        worker->start();
        worker->detach();
        return worker;
}

//============================================================================
// Author      : Soheil Abbasloo (abbasloo@cs.toronto.edu)
// Version     : V3.0
//============================================================================

/*
  MIT License
  Copyright (c) 2022 Soheil Abbasloo

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:
  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <fstream>
#include <iostream>
using namespace std;

#include <pthread.h>
#include <sched.h>
#include <sys/types.h> // needed for socket(), uint8_t, uint16_t, uint32_t
#include "flow.h"

static pthread_mutex_t lockit;      // 互斥锁

#include <math.h>
#include <time.h>
#include <unistd.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <deque>

//Shared Memory ==> Communication with RL-Module -----*
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/types.h>

//-------------------------*
#define OK_SIGNAL 99999
#define TARGET_MARGIN 100 //100% ==> Full Target!
#define TARGET_CHANGE_TIME \
    8 //Unit: minutes (Each TARGET_CHANGE_TIME minutes chagne the target)
#define TARGET_CHANGE_STEP 25

static int   shmid;
static key_t key = 123456;
static char* shared_memory;
static int   shmid_rl;
static key_t key_rl = 12345;
static char* shared_memory_rl;
static int   shmem_size = 2048; //Shared Memory size: 2KBytes
static int   key1 = 0;
static int   key2 = 0;
//----------------------------------------------------*
typedef unsigned int u32;
typedef int          s32;
typedef uint64_t     u64;
typedef int64_t      i64;
typedef uint8_t      u8;

static bool         send_traffic   = true;
static unsigned int duration       = 0;         //If not zero, it would be the total duration for sending out the traffic.

static uint64_t       start_of_client;
static bool           done  = false;           //Flow Completion Time
static uint64_t start_timestamp;

static int    actor_id;
static char*  scheme;
static char*  path;
static char*  env;
static char*  interpreter;
static bool   got_message   = 0;
static int    target_ratio  = 150;
static u32    report_period = 5;  //5ms
static int    flows_num     = 1;

#define FLOW_NUM 1
#define MAX_FLOW_NUM 10
static int sock;
static int sock_for_cnt[MAX_FLOW_NUM];

struct tcp_sage_info
{
    u8 state;
    u8 ca_state;
    u8 retransmits;
    u8 probes;
    u8 backoff;
    u8 options;
    u8 snd_wscale : 4, rcv_wscale : 4;
    u8 delivery_rate_app_limited : 1;

    u32 rto;
    u32 ato;
    u32 snd_mss;
    u32 rcv_mss;

    u32 unacked;
    u32 sacked;
    u32 lost;
    u32 retrans;
    u32 fackets;

    /* Times. */
    u32 last_data_sent;
    u32 last_ack_sent; /* Not remembered, sorry. */
    u32 last_data_recv;
    u32 last_ack_recv;

    /* Metrics. */
    u32 pmtu;
    u32 rcv_ssthresh;
    u32 rtt;
    u32 rttvar;
    u32 snd_ssthresh;
    u32 snd_cwnd;
    u32 advmss;
    u32 reordering;

    u32 rcv_rtt;
    u32 rcv_space;

    u32 total_retrans;

    u64 pacing_rate;
    u64 max_pacing_rate;
    u64 bytes_acked;    /* RFC4898 tcpEStatsAppHCThruOctetsAcked */
    u64 bytes_received; /* RFC4898 tcpEStatsAppHCThruOctetsReceived */
    u32 segs_out;       /* RFC4898 tcpEStatsPerfSegsOut */
    u32 segs_in;        /* RFC4898 tcpEStatsPerfSegsIn */

    u32 notsent_bytes;
    u32 min_rtt;
    u32 data_segs_in;  /* RFC4898 tcpEStatsDataSegsIn */
    u32 data_segs_out; /* RFC4898 tcpEStatsDataSegsOut */

    u64 delivery_rate;

    u64 busy_time;      /* Time (usec) busy sending data */
    u64 rwnd_limited;   /* Time (usec) limited by receive window */
    u64 sndbuf_limited; /* Time (usec) limited by send buffer */

    u32 delivered;
    u32 delivered_ce;

    u64 bytes_sent;    /* RFC4898 tcpEStatsPerfHCDataOctetsOut */
    u64 bytes_retrans; /* RFC4898 tcpEStatsPerfOctetsRetrans */
    u32 dsack_dups;    /* RFC4898 tcpEStatsStackDSACKDups */
    u32 reord_seen;    /* reordering events seen */

} 
static sage_info;


struct sTrace
{
    double time;
    double bw;
    double minRtt;
};
struct sInfo
{
    sTrace* trace;
    int     sock;
    int     num_lines;
};


static int          delay_ms;
static int          client_port;
static sTrace* trace;

#define DBGSERVER 0

#define TARGET_RATIO_MIN 100
#define TARGET_RATIO_MAX 1000

#define TCP_CWND_CLAMP 42
#define TCP_CWND 43
#define TCP_DEEPCC_ENABLE 44
#define TCP_CWND_CAP 45
#define TCP_DEEPCC_INFO 46 /* Get Congestion Control (optional) DeepCC info */
#define TCP_CWND_MIN 47
#define TCP_DEEPCC_PACING_COEF 58
#define TCP_DEEPCC_PACING_TYPE 59

#define PACE_WITH_MIN_RTT 0
#define PACE_WITH_SRTT 1

#define MAX_32Bit 0x7FFFFFFF

//Make sure we don't have (int32) overflow!
double   mul(double a, double b);
uint64_t raw_timestamp(void);
uint64_t timestamp_begin(bool set);
uint64_t timestamp_end(void);
uint64_t initial_timestamp(void);
uint64_t timestamp(void);

//Start server
void start_server(int flow_num, int client_port);

//thread functions
void* DataThread(void*);
void* CntThread(void*);
void* TimerThread(void*);
void* MonitorThread(void*);

//Print usage information
void usage();

void handler(int sig);

template <class T>
class dq_sage
{
    std::deque<T>* dq;
    T              default_max;
    u32            size;
    std::deque<T>* dq_min; // 双端队列, 维护窗口内的最值
    std::deque<T>* dq_max;
    //std::deque<double>* dq_avg;
    double average;
    u32    length;

public:
    dq_sage(u32 size)
    {
        init(size);
    };
    void init(u32 size, T default_max) // 初始化 size 并设置 max 值
    {
        this->size        = size;
        this->defualt_max = default_max;
        dq                = new std::deque<T>;
        dq_min            = new std::deque<T>;
        dq_max            = new std::deque<T>;
        //dq_avg = new std::deque<double>;
        this->average = 0;
    };
    void init(u32 size) // 初始化 size 并设置 max 值为 100Mbps
    {
        this->size        = size;
        dq                = new std::deque<T>;
        this->default_max = (T)100; //100Mbps
        dq_min            = new std::deque<T>;
        dq_max            = new std::deque<T>;
        //dq_avg = new std::deque<double>;
        this->average = 0;
    };
    T get_min()
    {
        return (this->dq_min->size())
                   ? this->dq_min->front()
                   : 1e6; // dq_min 里面有值, 取 dq_min 的第一个元素, 否则返回 1e6
    }
    T get_max()
    {
        return (this->dq_max->size()) ? this->dq_max->front() : 0;
    }
    double get_avg()
    {
        return this->average;
    }
    T get_sum()
    {
        return (T)(get_avg() * this->dq->size()); // avg * size
    }
    int add(T entry) // 添加一个元素
    {
        T   new_min = get_min();
        T   new_max = get_max();
        u32 len     = this->dq->size();
        if (entry < new_min)
        {
            new_min = entry; // 新的最小值
        }
        if (entry > new_max)
        {
            new_max = entry; // 新的最大值
        }

        if (len >= this->size) // 元素已满
        {
            T to_be_removed = this->dq->back(); // 获取双端队列的最后一个元素
            this->dq->pop_back();               // 去除这个元素
            this->average =
                (this->average * len - (double)to_be_removed + (double)entry) /
                (len); // 计算新的 average

            if (to_be_removed == get_min()) // 去除的是最小值
            {
                new_min = min(); // 寻找最小值
                if (entry < new_min)
                    new_min = entry;
            }
            this->dq_min->pop_back(); // 尾出队 dq_min 最后一个元素
            this->dq_min->push_front(new_min); // 首入队 dq_min 到第一个元素

            if (to_be_removed == get_max()) // 处理最大值
            {
                new_max = max();
                if (entry > new_max)
                    new_max = entry;
            }
            this->dq_max->pop_back();
            this->dq_max->push_front(new_max);

            //this->dq->pop_back();
            this->dq->push_front(entry);
        }
        else // 如果队列未满, 直接入队
        {
            this->average =
                (len) ? (this->average * len + (double)entry) / (len + 1)
                      : entry;
            this->dq_min->push_front(new_min);
            this->dq_max->push_front(new_max);
            this->dq->push_front(entry);
        }
    };
    T max()
    {
        T                                max       = 0;
        int                              occupancy = 0;
        typename std::deque<T>::iterator it;
        for (it = this->dq->begin(); it != this->dq->end(); it++)
        {
            if (max < *it)
            {
                max = *it;
            }
            //occupancy++;
        }
        return max;
    };
    T min()
    {
        T                                min = 1e6;
        typename std::deque<T>::iterator it;
        for (it = this->dq->begin(); it != this->dq->end(); it++)
        {
            if (min > *it)
            {
                min = *it;
            }
        }
        return min;
    };
    T sum()
    {
        T                                sum = 0;
        typename std::deque<T>::iterator it;
        for (it = this->dq->begin(); it != this->dq->end(); it++)
        {
            sum += *it;
        }
        return sum;
    }
    T avg()
    {
        T                                sum     = 0;
        u32                              counter = 0;
        typename std::deque<T>::iterator it;
        for (it = this->dq->begin(); it != this->dq->end(); it++)
        {
            sum += *it;
            counter++;
        }
        return (counter) ? (T)sum / counter : 0;
    }
    void std(T& mean, T& std)
    {
        mean                                     = avg();
        T                                var     = 0;
        u32                              counter = 0;
        typename std::deque<T>::iterator it;
        for (it = this->dq->begin(); it != this->dq->end(); it++)
        {
            var += (mean - *it) * (mean - *it);
            counter++;
        }
        std = var / counter;
        std = sqrt(std);
    }
};

#define EVALUATION 1
#define DS_VERSION 820

#include <sys/select.h>
#include <cstdlib>
#include <netdb.h> 
#include <stdio.h>
#include "define.h"

/* 超参数 */
//
#define WAIT_FOR_ACTION_MAX_ms 200

// cwnd 运算后取整
#define FLOORING 0
#define ROUNDING 1
#define CEILING 2
#define NONE 3
#define ROUNT_TYPE CEILING

#define TURN_ON_SAFETY 0
#define SAFETY_MARGIN 3 // 10

#define ACCUMULATE_CWND 1

#define SHORT_WIN 10
#define MID_WIN 200
#define LONG_WIN 1000

#define ESTIMATE 0
#define ACCURACY 10000.0 
            
// 1000.0 //100.0 //100.0 //10000.0    
// Final Cwnd precision: 1/ACCURACY

#define BDP_SRTT 1
#define BDP_MINRTT 2
#define UPPERBOUND_TYPE BDP_MINRTT

//#define SLOW_START_MANUAL
// Followings only will be used if SLOW_START_MANUAL is defined.
#define SLOW_START_STEP_WAIT_TIME 1     // n x rtt
#define SLOW_START_INIT_STATE false     // false
#define SLOW_START_TIME_MS 1000         // ms
#define SLOW_START_TRANSITION_STEPS 1   // 10
#define SLOW_START_COEF 2.0             // 2.0 //1.15
#define SLOW_START_CUTOFF 1000          // 0x10000000
#define SLOW_START_MAX_BW_IDLE_CUTOFF 1 // 3 //0x10000000

#define WIN_SIZE 500
//#define WIN_SIZE 10000

#define PACE_ENABLE 1
#define PACE_TYPE PACE_WITH_SRTT
#define PACE_COEF 100

#define MAX_CWND 20000
#define CWND_INIT 10
#define MIN_CWND 4 // CWND_INIT //1
#define STEPS_UPDATE_MAX_CWND_EFF 10
#define MAX_CWND_EFF_INIT 100
#define BW_NORM_FACTOR \
    100 // 100 Mbps will be used to normalize throughput signal

FILE* testing_;
enum fsm
{
    state_DRL  = 1,
    state_Fair = 2
};

fsm cc_state   = state_DRL;
int save       = 1;
u32 iterations = 0;
u64 send_begun;

void start_server_pantheon(int client_port);

int main(int argc, char** argv)
{
    DBGPRINT(DBGSERVER, 4, "Main\n");
    if (argc != 7)
    {
        DBGERROR("argc:%d\n", argc);
        for (int i = 0; i < argc; i++)
            DBGERROR("argv[%d]:%s\n", i, argv[i]);
        usage();
        return 0;
    }
    // ./sage [env] [python interpreter] [port] [path to run.py] [scheme: pure] [actor_id]"

    srand(raw_timestamp());         /* 通过时间戳初始化随机种子 */

    signal(SIGSEGV, handler);       // install our handler
    signal(SIGTERM, handler);       // install our handler
    signal(SIGABRT, handler);       // install our handler
    signal(SIGFPE,  handler);       // install our handler
    signal(SIGKILL, handler);       // install our handler

    env           = argv[1];            // 环境变量
    interpreter   = argv[2];            // python 解释器
    client_port   = atoi(argv[3]);
    path          = argv[4];
    scheme        = argv[5];            // "pure";
    actor_id      = atoi(argv[6]);
 
    DBGPRINT(DBGSERVER, 5,
             "********************************************************\n "
             "starting the server ...\n");
    start_server_pantheon(client_port);         /* 运行 server */

    shmdt(shared_memory);               /* 断开共享内存 */
    shmctl(shmid, IPC_RMID, NULL);     /* 删除共享内存 */
    shmdt(shared_memory_rl);
    shmctl(shmid_rl, IPC_RMID, NULL);

    return 0;
}


/* 调用格式 */
void usage()
{
    DBGERROR(
        "./sage [env] [python interpreter] [port] [path to run.py] [scheme: pure] [actor_id]\n");
}



/* 启动 server, 被动接受连接的一方 */
void start_server_pantheon(int client_port)
{
    cFlow* flows;
    sInfo* info;
    int    num_lines = 0;

    info  = new sInfo;
    flows = new cFlow;

    if (flows == NULL || info == NULL)
    {
        DBGERROR("flow or info generation failed\n");
        return;
    }

    // threads
    pthread_t data_thread;  // 数据进程
    pthread_t cnt_thread;   // 计数进程
    pthread_t timer_thread; // 计时进程

    /******
     * 1. 设置 socket
     ******/
    // Server address
    struct sockaddr_in server_addr;
    // Client address
    struct sockaddr_in client_addr;
    // Controller address
    memset(&server_addr, 0, sizeof(server_addr));
    // IP protocol
    server_addr.sin_family = AF_INET; // 协议: ipv4/tcp
    // Listen on "0.0.0.0" (Any IP address of this host)
    server_addr.sin_addr.s_addr = INADDR_ANY; // 监听全部 ip 地址
    // Specify port number
    server_addr.sin_port = htons(client_port); // 监听的端口

    // Init socket
    if ((sock = socket(PF_INET, SOCK_STREAM, 0)) < 0) // 初始化 socket
    {
        DBGERROR("sockopt: %s\n", strerror(errno));
        return;
    }

    // Set reuse
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse,
                   sizeof(reuse)) < 0)
        perror("setsockopt(SO_REUSEADDR) failed");

    // Bind socket on IP:Port
    if (bind(sock, (struct sockaddr*)&server_addr,
             sizeof(struct sockaddr)) < 0) // 绑定 ip:port (any:port)
    {
        DBGERROR("[Actor %d] bind error srv_ctr_ip: 000000: %s\n", actor_id,
                 strerror(errno));
        close(sock);
        return;
    }

    // Set CC scheme
    if (scheme) // 指定拥塞控制算法
    {
        if (setsockopt(sock, IPPROTO_TCP, TCP_CONGESTION, scheme,
                       strlen(scheme)) < 0) // 指定拥塞控制算法
        {
            DBGMARK(DBGSERVER, 5, "Scheme (%s) Failed \n", scheme);
            DBGERROR("TCP congestion doesn't exist: %s\n", strerror(errno));
            return;
        }
    }

    char container_cmd[500];
    char tmp_cmd[500];
    char pre_cmd[500];
    snprintf(tmp_cmd, sizeof(tmp_cmd), " ");
    snprintf(pre_cmd, sizeof(pre_cmd), " ");
    char cmd[1000];
    char final_cmd[1000];

    info->trace     = trace;
    info->num_lines = num_lines;

    /******
     * 2. Setup Shared Memory, 设置共享内存
     ******/
    key    = (key_t)(actor_id * 10000 + rand() % 10000 + client_port);
    key_rl = (key_t)(actor_id * 10000 + rand() % 10000 + client_port);

    if ((shmid = shmget(key, shmem_size, IPC_CREAT | 0666)) < 0)
    {
        printf("Error getting shared memory id");
        return;
    }

    // Attached shared memory
    if ((shared_memory = (char*)shmat(shmid, NULL, 0)) == (char*)-1)
    {
        printf("Error attaching shared memory id");
        return;
    }

    // Setup shared memory, 11 is the size
    if ((shmid_rl = shmget(key_rl, shmem_size, IPC_CREAT | 0666)) < 0)
    {
        printf("Error getting shared memory id");
        return;
    }

    // Attached shared memory
    if ((shared_memory_rl = (char*)shmat(shmid_rl, NULL, 0)) == (char*)-1)
    {
        printf("Error attaching shared memory id");
        return;
    }

    /**
     * 3. 运行 tcpactor.py
     * mode 0: 评估
     * 实际上只是将 mode 传递给了 tcpactor.py 处理, 交付共享内存用于通信
     */
    DBGERROR("Starting RL in Evalution Mode(0) ...\n%s", cmd);
    snprintf(cmd, sizeof(cmd),
            "%s %s/tcpactor.py "
            "--base_path=%s --mode=0 --mem_r=%d --mem_w=%d --id=%d "
            "--flows=%d --bw=%d&",
            interpreter,
            path, path, (int)key, (int)key_rl, actor_id, flows_num, 0);


    putenv(env);
    system(cmd); /* 执行指令, 等待 tcpactor.py 加载模型 */

    /* /home/zhangbochun/anaconda3/envs/sage/bin/python
     * /home/zhangbochun/pantheon-3/third_party/sage/sage_rl/rl_module/tcpactor.py
     * --base_path=/home/zhangbochun/pantheon-3/third_party/sage/sage_rl/rl_module
     * --mode=0 --mem_r=60817 --mem_w=60611 --id=1 --flows=1 --bw=24&
     */

    /**
     * 4. 通过 shared memory 与 tcpactor.py 交互, 确认 actor 是否加载完成
     * Wait to get OK signal (alpha=OK_SIGNAL)
     */
    DBGERROR("(Actor %d) Waiting for RL Module ...\n", actor_id);

    bool  got_ready_signal_from_rl = false;
    int   signal;
    char* num;
    char* alpha;
    char* save_ptr;
    int   signal_check_counter = 0;

    while (!got_ready_signal_from_rl)
    {
        // Get alpha from RL-Module
        signal_check_counter++;
        num   = strtok_r(shared_memory_rl, " ",
                       &save_ptr); /* 线程安全的字符串分割函数 */
        alpha = strtok_r(
            NULL, " ",
            &save_ptr); // 分割 shared_memory_rl, 分隔符 "", 保存指针到 save_ptr
        if (num != NULL && alpha != NULL) // 获取到了数据
        {
            signal = atoi(alpha);
            if (signal == OK_SIGNAL)
            {
                got_ready_signal_from_rl = true; // 已经完成模型加载
            }
            else
            {
                usleep(10000); // 否则睡眠 10ms
            }
        }
        else
        {
            usleep(10000); // 没有读取到数据同样睡眠 10ms
        }
        if (signal_check_counter > 6000) // 1min 还没加载好, 退出
        {
            DBGERROR("After 1 minute, no response (OK_Signal) from the Actor "
                     "%d is received! We are going down down down ...\n",
                     actor_id);
            return;
        }
    }

    DBGERROR("(Actor %d) RL Module is Ready. Let's Start ... \n\n", actor_id);
    usleep(actor_id * 10000 + 10000);
    initial_timestamp();

    /**
     * 5. 开始监听
     */
    FILE* filep;
    char  line[4096];

    // Start listen
    fd_set rset;
    FD_ZERO(&rset);
    listen(sock, 10);        // sock 监听连接请求, 队列中能够存放 10 个连接
    FD_SET(sock, &rset);           // 将描述符 sock[i] 加入到集合 rset

    // Timeout {2min}, if something goes wrong! (Maybe  mahimahi error ...!)
    // 轮询检查读写性, ndfs 设置为 maxfdp+1 (Linux 规定), 将可读性信息写入 rest, 阻塞 2min 等待可读变化
    struct timeval timeout;
    timeout.tv_sec  = 30;           // 2 * 60;
    timeout.tv_usec = 0;
    int rc          = select(sock + 1, &rset, NULL, NULL, &timeout);

    /**********************************************************/
    /* Check to see if the select call failed.                */
    /**********************************************************/
    if (rc < 0)
    {
        DBGERROR("=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-==-=-=-=-=-=-=- "
                 "select() failed =-=-=-=-=-=--=-=-=-=-=\n");
        return;
    }

    /**********************************************************/
    /* Check to see if the X minute time out expired.         */
    /**********************************************************/
    if (rc == 0)
    {
        DBGERROR("=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-==-=-=-=-=-=-=- "
                 "select() Timeout! =-=-=-=-=-=--=-=-=-=-=\n");
        return;
    }

    /* 6. 接收 connect 请求
     * 接受 flow[id] 的连接请求
     * 为 flow[id] 生成子线程,  起始地址 DataThread,   参数 flows, 这里有 bug, 后面的 flow 会覆盖前面 flow 的描述符
     * 所有流共用一个计数子线程, 起始地址 CntThread,    参数为 info
     * 所有流共用一个计时子线程, 起始地址 TimerThread,  参数为 info
     */
    int sin_size = sizeof(struct sockaddr_in);
    if (FD_ISSET(sock, &rset))          // 检测 sock 是否在可读的集合内
    {
        int value = accept(sock, (struct sockaddr*)&client_addr,
                           (socklen_t*)&sin_size); // 接受连接请求

        DBGERROR("(Actor %d) receive something...\n", actor_id);
        if (value < 0)
        {
            perror("accept error\n");
            DBGERROR("sockopt: %s\n", strerror(errno));
            DBGERROR("sock::%d, index:%d\n", sock, 0);
            close(sock);
            return;
        }
        DBGERROR("(Actor %d) accept connection, accept = %d...\n", actor_id, value);


        start_timestamp = timestamp();

        sock_for_cnt[0]        = value;             // 记录新的连接
        flows[0].flowinfo.sock = value;             // 将新的连接写入 value
        flows[0].dst_addr      = client_addr;
        if (pthread_create(&data_thread, NULL, DataThread,
                           (void*)&flows[0]) < 0) // 创建 data 子线程
        {
            DBGERROR("could not create thread\n");
            close(sock);
            return;
        }

        if (pthread_create(&cnt_thread, NULL, CntThread, (void*)info) < 0)
        {
            DBGERROR("could not create control thread\n");
            close(sock);
            return;
        }

        if (pthread_create(&timer_thread, NULL, TimerThread, (void*)info) < 0)
        {
            DBGERROR("could not create timer thread\n");
            close(sock);
            return;
        }

        DBGERROR("(Actor %d) Server is Connected to the client...\n", actor_id);
    }
    // fclose(testing_);
    pthread_join(data_thread, NULL);        /* 等待子线程结束 */
}

void* TimerThread(void* information)
{
    uint64_t start =
        timestamp(); // 起始时间, duration 的含义: 所有流发送的总时间
    unsigned int elapsed;
    if ((duration != 0)) // 经过一段时间 duration, 将 send_traffic 设置为 false
    {
        while (send_traffic)
        {
            sleep(1);
            elapsed = (unsigned int)((timestamp() - start) / 1000000); // unit s
            if (elapsed > duration)
            {
                send_traffic = false;
            }
        }
    }

    return ((void*)0);
}

void* CntThread(void* information)
{
    int    ret1;
    double min_rtt_        = 0.0;
    double pacing_rate     = 0.0;
    double lost_bytes      = 0.0;
    double lost_rate       = 0.0;
    double srtt_ms         = 0.0;
    double snd_ssthresh    = 0.0;
    double packets_out     = 0.0;
    double retrans_out     = 0.0;
    double max_packets_out = 0.0;

    int  reuse                = 1;
    int  pre_id               = 9230;
    int  pre_id_tmp           = 0;
    int  msg_id               = 657;
    bool got_alpha            = false;
    bool slow_start_passed    = SLOW_START_INIT_STATE;
    int  slow_start_tran_step = 0;
    int  slow_start_counter   = 0;

    if (setsockopt(sock_for_cnt[0], IPPROTO_TCP, TCP_NODELAY, &reuse,
                   sizeof(reuse)) < 0)
    {
        DBGERROR("ERROR: set TCP_NODELAY option %s\n", strerror(errno));
        return ((void*)0);
    }
    // Enable DeepCC Style pacing on this socket:

    int enable_deepcc = 1;
    if (PACE_ENABLE == 1)
        // With Pacing
        enable_deepcc = 2;
    if (setsockopt(sock_for_cnt[0], IPPROTO_TCP, TCP_DEEPCC_ENABLE,
                   &enable_deepcc, sizeof(enable_deepcc)) < 0)
    {
        DBGERROR("CHECK KERNEL VERSION (0514+) ;CANNOT ENABLE DEEPCC %s\n",
                 strerror(errno));
        return ((void*)0);
    }

    int deepcc_pacing_type = PACE_TYPE;
    if (setsockopt(sock_for_cnt[0], IPPROTO_TCP, TCP_DEEPCC_PACING_TYPE,
                   &deepcc_pacing_type, sizeof(deepcc_pacing_type)) < 0)
    {
        DBGERROR("CHECK KERNEL VERSION (006+) ;CANNOT SET PACING TYPE %s\n",
                 strerror(errno));
        return ((void*)0);
    }
    int deepcc_pacing_coef = PACE_COEF;
    if (setsockopt(sock_for_cnt[0], IPPROTO_TCP, TCP_DEEPCC_PACING_COEF,
                   &deepcc_pacing_coef, sizeof(deepcc_pacing_coef)) < 0)
    {
        DBGERROR("CHECK KERNEL VERSION (006+) ;CANNOT SET PACING COEF %s\n",
                 strerror(errno));
        return ((void*)0);
    }

    char     message[1000];
    char*    shared_memory_rl2 = (char*)malloc(strlen(shared_memory_rl));
    char     message_summarized[1000];
    char*    num;
    char*    alpha;
    char*    save_ptr;
    int      got_no_zero = 0;
    uint64_t t0, t1, t2, t3, diff_fairness_time, t4, t5, t1_pre, start_time,
        time_tmp;
    t2 = timestamp();


    char file_name[1000];

    unsigned int sage_info_length      = sizeof(sage_info);
    u64          total_bytes_acked_pre = 0, max_delivary_rate = 0,
        pre_max_delivary_rate = 0, min_rtt_us = 1, pre_bytes_sent = 0,
        pre_pkt_dlv = 0, pre_pkt_lost = 0, pre_rtt_ = 0;
    u64    max_sending_rate = 0;
    u32    pre_cwnd         = CWND_INIT;
    double max_rate = 0, delta_max_rate = 0, pre_max_rate = 0;
    u32    max_cwnd_effective = MAX_CWND_EFF_INIT;
    u32    max_cwnd_eff_srtt  = MAX_CWND_EFF_INIT;
    double my_q_delay         = 0;
    double others_q_delay     = 0;
    double reward             = 0.0;
    double acked_sent_rate    = 0;

    dq_sage<u64> sending_rates(
        WIN_SIZE * 10); // 使用不同粒度的数据, 例如 10/200/1000 个样本计算
    dq_sage<u64> delivery_rates(
        WIN_SIZE * 10); // 所谓粒度是指: 若干个样本数量, 这些容器都是同步更新的
    dq_sage<u64> rtts(WIN_SIZE);

    dq_sage<double> ack_snt_s(SHORT_WIN);
    dq_sage<double> ack_snt_m(MID_WIN);
    dq_sage<double> acl_snt_l(LONG_WIN);

    dq_sage<double> rtt_s(SHORT_WIN);
    dq_sage<double> rtt_m(MID_WIN);
    dq_sage<double> rtt_l(LONG_WIN);

    dq_sage<double> lost_s(SHORT_WIN); // x = lost pkts / 1000
    dq_sage<double> lost_m(MID_WIN);
    dq_sage<double> lost_l(LONG_WIN);

    dq_sage<double> inflight_s(SHORT_WIN); // x = inflight pkts / 1000
    dq_sage<double> inflight_m(MID_WIN);
    dq_sage<double> inflight_l(LONG_WIN);

    dq_sage<double> delivered_s(SHORT_WIN);
    dq_sage<double> delivered_m(MID_WIN);
    dq_sage<double> delivered_l(LONG_WIN);

    dq_sage<double> thr_s(SHORT_WIN);
    dq_sage<double> thr_m(MID_WIN);
    dq_sage<double> thr_l(LONG_WIN);

    dq_sage<double> rtt_rate_s(SHORT_WIN);
    dq_sage<double> rtt_rate_m(MID_WIN);
    dq_sage<double> rtt_rate_l(LONG_WIN);

    dq_sage<double> rtt_var_s(SHORT_WIN);
    dq_sage<double> rtt_var_m(MID_WIN);
    dq_sage<double> rtt_var_l(LONG_WIN);

    dq_sage<double> loss_win(50);
    dq_sage<double> delta_time_win(50);

    dq_sage<u64> sent_db(100);
    dq_sage<u64> sent_dt(100);
    dq_sage<u64> dlv_db(100);
    dq_sage<u64> dlv_dt(100);
    dq_sage<u64> loss_db(100);
    dq_sage<u64> uack_db(100);

    double sr_w_mbps = 0.0, dr_w_mbps = 0.0, l_w_mbps = 0.0, pre_dr_w_mbps = 0;
    //
    dq_sage<double> dr_w(200);  // 2 Seconds?    //dr_w(4000); //40 Seconds
    dq_sage<double> rtt_w(200); // 2 Seconds?

    double pre_dr_w_max = 1.0;
    double dr_w_max     = 1.0;

    t0                  = timestamp();
    t1_pre              = t0;
    double cwnd_precise = pre_cwnd;
    double cwnd_rate    = 1.0;

    u32    pre_lost_packets = 0;
    double real_lost_rate   = 0.0;
    start_time              = timestamp();
    u64 total_lost          = 0;

    bool first_time_hitting_max = 0;

    u64 dt     = 0;
    u64 dt_pre = timestamp();

    /* 1. 遍历流
     * t0: cntThread init time; t1: , t2: , t3: ;
     * dq_sage<T> 是一个模板, 收集不同类型数据, 然后自动计算 average 值
     * while (send_traffic): -> 这个值有 TimerThread 管理
     *   for: 遍历流 -> 其实只有 1 个流
     *     1.1 -> cc_state == state_DRL
     *       while (!got_no_zero && send_traffic): -> 不断循环, 知道成功获取数据
     *         获取最新确认的字节数, acked > 0
     *           -> 从内核读取 sage_info, 将数据入队 dq_sage, 组装环境状态
     *           -> 计算环境奖励
     *           -> 将环境数据写入 shared_memory
     *           -> 等待 actor 输出数值
     *           -> 根据数值计算 cwnd, 取整数, 写入内核
     *     1.2 -> cc_state != state_DRL
     *       set cwnd -> 4
     */
    while (send_traffic)
    {
        if (cc_state == state_DRL)
        {
            got_no_zero = 0;
            t3          = timestamp();
            while (!got_no_zero && send_traffic)
            {
                t1 = timestamp();
                if ((t1 - t0) < (report_period * 1000))
                {
                    usleep(report_period * 1000 - t1 + t0);
                }
                ret1 = getsockopt(
                    sock_for_cnt[0], SOL_TCP, TCP_INFO, (void*)&sage_info,
                    &sage_info_length); // 从 socket 中获取 TCP_INFO 字段
                if (ret1 < 0)
                {
                    DBGMARK(0, 0,
                            "setsockopt: for index:%d "
                            "TCP_INFO ... %s (ret1:%d)\n",
                            0, strerror(errno), ret1);
                    return ((void*)0);
                }

                int bytes_acked_ =
                    (sage_info.bytes_acked -
                     total_bytes_acked_pre); // 计算这段时间内确认的字节数
                u64 s_db;
                u64 d_dp = (sage_info.delivered -
                            pre_pkt_dlv);            // Delivered Delta Packets
                u64 d_db = (d_dp)*sage_info.snd_mss; // Delivered Delta Bytes
                u64 l_db;
                pre_pkt_dlv = sage_info.delivered;

                if (bytes_acked_ > 0 || d_db > 0) // 确认了新的字节
                {
                    dt     = timestamp() - dt_pre; // 经过的时间
                    dt     = (dt > 0) ? dt : 1;
                    dt_pre = timestamp();
                    s_db   = sage_info.bytes_sent -
                           pre_bytes_sent; // Sent Delta Bytes, 期间发送的字节数
                    l_db = (sage_info.lost > pre_pkt_lost)
                               ? (sage_info.lost - pre_pkt_lost) *
                                     sage_info.snd_mss
                               : 0; // 期间丢失的报文数量
                    pre_pkt_lost = sage_info.lost;
                    uack_db.add((u64)sage_info.unacked *
                                sage_info.snd_mss); // 尚未确认的 ack 数量
                    sent_db.add(s_db);
                    dlv_db.add(d_db);
                    loss_db.add(l_db);
                    sent_dt.add(dt);
                    dlv_dt.add(dt); // It is redundant!
                    u64 dt_sum = sent_dt.sum();
                    sr_w_mbps  = (double)8 * sent_db.sum() / dt_sum;
                    dr_w_mbps =
                        (double)8 * dlv_db.sum() / dt_sum; // 计算时间窗内的带宽
                    l_w_mbps = (double)8 * loss_db.sum() / dt_sum;
                    dr_w.add(dr_w_mbps);
                    dr_w_max = dr_w.max();
                    if (dr_w_max == 0.0)
                        dr_w_max = 1;
                    rtt_w.add((double)sage_info.rtt / 100000.0);

                    double now        = timestamp();
                    double time_delta = (double)(now - t0) / 1000000.0;
                    // we divide to mss instead of mss+60! To make sure that
                    // we always have the right integer value! double
                    // acked_rate = bytes_acked_/(sage_info.snd_mss);
                    total_bytes_acked_pre = sage_info.bytes_acked;
                    double acked_rate     = bytes_acked_;
                    acked_rate            = acked_rate / time_delta;

                    now = now / 1000000.0;
                    double sending_rate =
                        (double)(sage_info.bytes_sent - pre_bytes_sent); //=
                    //(double)sage_info.snd_cwnd;
                    pre_bytes_sent = sage_info.bytes_sent;

                    double rtt_rate;
                    delivery_rates.add(sage_info.delivery_rate);

                    max_delivary_rate = dr_w_max * 1e6 / 8;

                    min_rtt_us = sage_info.min_rtt;

                    if (max_delivary_rate > 0)
                    {
                        max_rate =
                            (double)sage_info.delivery_rate / max_delivary_rate;
                        double tmp;
                        int    tmp_;
                        tmp = ((double)max_delivary_rate /
                               (sage_info.snd_mss + 60)) *
                              (sage_info.rtt) / 1e6;
                        max_cwnd_eff_srtt = (u32)tmp;
                        if (PACE_TYPE == PACE_WITH_MIN_RTT)
                        {
                            tmp = ((double)max_delivary_rate /
                                   (sage_info.snd_mss + 60)) *
                                  (min_rtt_us) / 1e6;
                        }
                        tmp_               = (int)tmp;
                        max_cwnd_effective = tmp_;

                        my_q_delay     = (double)(1e6 * sage_info.snd_cwnd *
                                              (sage_info.snd_mss + 60) /
                                              max_delivary_rate);
                        others_q_delay = (double)(my_q_delay / min_rtt_us);

                        if (max_cwnd_effective)
                            lost_rate =
                                (double)(sage_info.lost) / max_cwnd_effective;

                        if (sage_info.lost > pre_lost_packets)
                        {
                            loss_win.add((double)(8 *
                                                  abs((int)(sage_info.lost -
                                                            pre_lost_packets)) *
                                                  sage_info.snd_mss) /
                                         1e6);
                            total_lost +=
                                8 *
                                abs((int)(sage_info.lost - pre_lost_packets)) *
                                sage_info.snd_mss;
                        }
                        else
                        {
                            loss_win.add(0.0);
                        }
                        delta_time_win.add(time_delta);

                        real_lost_rate             = 0.0;
                        double total_time_win_loss = delta_time_win.sum();
                        if (total_time_win_loss > 0.0)
                        {
                            real_lost_rate =
                                loss_win.sum() / total_time_win_loss; // Mbps
                        }

                        pre_lost_packets = sage_info.lost;
                    }
                    else
                    {
                        acked_rate       = 0;
                        lost_rate        = 0;
                        pre_lost_packets = 0;
                        real_lost_rate   = 0;
                    }
                    if (pre_max_delivary_rate > 0)
                    {
                        delta_max_rate =
                            (double)max_delivary_rate / pre_max_delivary_rate;
                    }
                    else
                    {
                        delta_max_rate = 0;
                    }
                    pre_max_delivary_rate = max_delivary_rate;

                    if (sage_info.rtt > 0)
                    {
                        sending_rate = sending_rate / time_delta; // Bps

                        sending_rates.add((u64)sending_rate);
                        max_sending_rate = sending_rates.max();
                        // rtt_rate =
                        // (double)sage_info.min_rtt/sage_info.rtt;
                        rtt_rate = (double)min_rtt_us /
                                   sage_info.rtt; // 最小 rtt / 当前 rtt
                        // time_delta =
                        // sage_info.min_rtt/(1000000*time_delta);
                        time_delta = (1000000 * time_delta) / sage_info.min_rtt;

                        acked_rate = acked_rate / max_sending_rate;
                        // lost_rate = lost_rate/max_sending_rate;
                    }
                    else
                    {
                        time_delta   = report_period;
                        sending_rate = 0;
                        rtt_rate     = 0.0;
                    }

                    double diff_rate;
                    // if (sage_info.delivery_rate>0)
                    if (max_sending_rate > 0)
                        diff_rate =
                            (double)sage_info.delivery_rate / max_sending_rate;
                    else
                        diff_rate = 0;

                    time_tmp = timestamp() - send_begun;
                    double tmp_deliv_rate_mbps =
                        (double)(sage_info.snd_mss * sage_info.delivered * 8) /
                        time_tmp;

                    double real_lost_rate_norm = 0;
                    if (max_delivary_rate)
                    {
                        real_lost_rate_norm = 1e6 * real_lost_rate /
                                              max_delivary_rate /
                                              8; // signal = loss
                        max_rate =
                            (double)sage_info.delivery_rate / max_delivary_rate;
                    }
                    else
                    {
                        real_lost_rate_norm = 0;
                        max_rate            = 0;
                    }

                    pre_max_rate = max_rate;

                    u64    s_db_tmp  = sent_db.sum();
                    u64    ua_db_tmp = uack_db.avg();
                    double cwnd_unacked_rate =
                        (s_db_tmp > 0)
                            ? (double)ua_db_tmp / s_db_tmp
                            : (double)
                                  ua_db_tmp; //(double)sage_info.unacked/sage_info.snd_cwnd;

                    double srate = (double)8 * sage_info.bytes_sent / time_tmp;
                    double drate = (double)8 * sage_info.delivered *
                                   sage_info.snd_mss / time_tmp;
                    double lrate = (double)total_lost / time_tmp; // Mbps
                    u64    cwnd_bits =
                        (u64)sage_info.snd_cwnd * sage_info.snd_mss * 8;
                    if (cwnd_bits == 0)
                        cwnd_bits++;

                    //---------------------------------------------------------------------------------------------------------------------------------------
                    /***
                     * Smoothed Versions ...
                     ***/
                    rtt_s.add((double)sage_info.rtt / 100000);
                    rtt_m.add((double)sage_info.rtt / 100000);
                    rtt_l.add((double)sage_info.rtt / 100000);

                    rtt_rate_s.add(rtt_rate);
                    rtt_rate_m.add(rtt_rate);
                    rtt_rate_l.add(rtt_rate);

                    rtt_var_s.add((double)sage_info.rttvar / 1000.0);
                    rtt_var_m.add((double)sage_info.rttvar / 1000.0);
                    rtt_var_l.add((double)sage_info.rttvar / 1000.0);

                    thr_s.add((double)sage_info.delivery_rate / 125000.0 /
                              BW_NORM_FACTOR);
                    thr_m.add((double)sage_info.delivery_rate / 125000.0 /
                              BW_NORM_FACTOR);
                    thr_l.add((double)sage_info.delivery_rate / 125000.0 /
                              BW_NORM_FACTOR);

                    inflight_s.add((double)sage_info.unacked / 1000.0);
                    inflight_m.add((double)sage_info.unacked / 1000.0);
                    inflight_l.add((double)sage_info.unacked / 1000.0);

                    lost_s.add((double)sage_info.lost / 100.0);
                    lost_m.add((double)sage_info.lost / 100.0);
                    lost_l.add((double)sage_info.lost / 100.0);

                    /**
                     * Soheil: Other interesting fields: potential input
                     * state candidates
                     * */
                    char message_extra[1000];
                    snprintf(
                        message_extra, sizeof(message_extra),
                        "     %.7f %.7f %.7f %.7f   %.7f %.7f    %d %u    "
                        "%.7f %.7f %.7f   %.7f %.7f %.7f   %.7f %.7f %.7f  "
                        "  %.7f %.7f %.7f    %.7f %.7f %.7f    %.7f %.7f "
                        "%.7f    %.7f %.7f %.7f   %.7f %.7f %.7f   %.7f "
                        "%.7f %.7f    %.7f %.7f %.7f    %.7f %.7f %.7f    "
                        "%.7f %.7f %.7f     %.7f %.7f %.7f    %.7f %.7f "
                        "%.7f    %.7f %.7f %.7f       %.7f %.7f %.7f    "
                        "%.7f %.7f %.7f    %.7f %.7f %.7f                  "
                        "   ",

                        // 2
                        (double)sage_info.rtt /
                            100000.0, /*sRTT in 100x (ms):e.g. 2 = 2x100=200
                                            ms*/
                        // 3
                        (double)sage_info.rttvar /
                            1000.0, /*var of sRTT in 1x (ms). */
                        // 4x
                        (double)sage_info.rto /
                            100000.0, /*retrans timeout in 100x (ms)*/
                        // 5x
                        (double)sage_info.ato /
                            100000.0, /*Ack timeout in 100x (ms)*/
                        // 6x
                        (double)sage_info.pacing_rate / 125000.0 /
                            BW_NORM_FACTOR, /*pacing rate 100x Mbps*/
                        // 7
                        (double)sage_info.delivery_rate / 125000.0 /
                            BW_NORM_FACTOR, /*del rate 100x Mbps*/
                        // 8x
                        sage_info.snd_ssthresh,
                        // 9
                        sage_info.ca_state, /*TCP_CA_Open=0 -> TCP_CA_Loss=4*/

                        // 10, 11, 12     13, 14, 15   16, 17, 18
                        rtt_s.get_avg(), rtt_s.get_min(), rtt_s.get_max(),
                        rtt_m.get_avg(), rtt_m.get_min(), rtt_m.get_max(),
                        rtt_l.get_avg(), rtt_l.get_min(), rtt_l.get_max(),
                        // 19, 20, 21     22, 23, 24   25, 26, 27
                        thr_s.get_avg(), thr_s.get_min(), thr_s.get_max(),
                        thr_m.get_avg(), thr_m.get_min(), thr_m.get_max(),
                        thr_l.get_avg(), thr_l.get_min(), thr_l.get_max(),
                        // 28, 29, 30     31, 32, 33   34, 35, 36
                        rtt_rate_s.get_avg(), rtt_rate_s.get_min(),
                        rtt_rate_s.get_max(), rtt_rate_m.get_avg(),
                        rtt_rate_m.get_min(), rtt_rate_m.get_max(),
                        rtt_rate_l.get_avg(), rtt_rate_l.get_min(),
                        rtt_rate_l.get_max(),
                        // 37, 38, 39     40, 41, 42   43, 44, 45
                        rtt_var_s.get_avg(), rtt_var_s.get_min(),
                        rtt_var_s.get_max(), rtt_var_m.get_avg(),
                        rtt_var_m.get_min(), rtt_var_m.get_max(),
                        rtt_var_l.get_avg(), rtt_var_l.get_min(),
                        rtt_var_l.get_max(),
                        // 46, 47, 48     49, 50, 51   52, 53, 54
                        inflight_s.get_avg(), inflight_s.get_min(),
                        inflight_s.get_max(), inflight_m.get_avg(),
                        inflight_m.get_min(), inflight_m.get_max(),
                        inflight_l.get_avg(), inflight_l.get_min(),
                        inflight_l.get_max(),
                        // 55, 56, 57     58, 59, 60   61, 62, 63
                        lost_s.get_avg(), lost_s.get_min(), lost_s.get_max(),
                        lost_m.get_avg(), lost_m.get_min(), lost_m.get_max(),
                        lost_l.get_avg(), lost_l.get_min(), lost_l.get_max());

                    int ret = snprintf(message, sizeof(message),
                            "%.7f %.7f %s  %.7f   %.7f %.7f   %.7f "
                            "%.7f    %.7f %.7f    %.7f %.7f   %.7f "
                            "%.7f   %.7f %.7f",
                            // 0x                        1x [2-63]
                            // (double)time_on_trace / 1e3, max_tmp,
                            (double)0, (double)0, message_extra,
                            // 64 x
                            dr_w_mbps - l_w_mbps,
                            // 65          66
                            time_delta, rtt_rate,
                            // 67                        68
                            l_w_mbps / BW_NORM_FACTOR, acked_rate,
                            // 69 70
                            (pre_dr_w_mbps > 0.0) ? (dr_w_mbps / pre_dr_w_mbps)
                                                  : dr_w_mbps,
                            (double)(dr_w_max * sage_info.min_rtt) /
                                (cwnd_bits),
                            // 71                                    72
                            (dr_w_mbps) / BW_NORM_FACTOR,
                            cwnd_unacked_rate, // sr_w_mbps/dr_w_max,
                            // 73
                            (pre_dr_w_max > 0.0) ? (dr_w_max / pre_dr_w_max)
                                                 : dr_w_max,
                            // 74
                            dr_w_max / BW_NORM_FACTOR,
                            // Soheil: If we should change the reward
                            // (giving pentalties, etc.), we will do it
                            // here and send the final reward to python
                            // code. 75x    76
                            reward,
                            (cwnd_rate > 0.0)
                                ? round(log2f(cwnd_rate) * 1000) / 1000.
                                : log2f(0.0001));

                    if (ret < 0)
                    {
                        DBGERROR("message trunck");
                        return ((void*)0);
                    }

                    char message2[1000];
                    ret = snprintf(message2, sizeof(message2), "%d %s", msg_id, message); // 补充 msg_id
                    
                    if (ret < 0)
                    {
                        DBGERROR("message trunck");
                        return ((void*)0);
                    }
                    
                    memcpy(shared_memory, message2,
                           sizeof(message2)); // 拷贝到共享内存

                    /**
                     * message 的格式:
                     * msg_id[0x], time_on_trace[1x], max_tmp[2x], [3 - 64 (测量值){5x, 6x, 7x, 9x}] [65x - 75 (测量值)]
                     * reward[76x], last_action [77] {78 维输入}
                     * 不需要的维度, 已经在对应位置标记 0/1/4/5/6/8/64/75
                     */

                    got_no_zero   = 1;
                    t0            = timestamp();
                    pre_rtt_      = sage_info.rtt;
                    pre_dr_w_mbps = dr_w_mbps;
                    pre_dr_w_max  = dr_w_max;
                    msg_id        = (msg_id + 1) % 1000;
                }
            }
            
            got_alpha                       = false;
            int    error_cnt                = 0;
            int    error2_cnt               = 0;
            double target_cwnd              = 4;
            u64    cwnd_tmp                 = 0;
            u64    cwnd_max_tmp             = 0;
            int    cnt_tmp                  = 0;
            int    tmp___                   = 0;
            int    show_log                 = 0;
            u64    action_get_time_start_ms = timestamp() / 1000;
            while (!got_alpha && send_traffic)
            {
                // Get alpha from RL-Module
                strcpy(
                    shared_memory_rl2,
                    shared_memory_rl); // to not destroy the original string 从共享内存拷贝到本地, 避免破坏共享内存中的内容
                num = strtok_r(shared_memory_rl2, " ", &save_ptr);

                alpha = strtok_r(NULL, " ", &save_ptr);
                if (num != NULL && alpha != NULL) // 读取数据成功
                {
                    if (show_log)
                    {
                        DBGERROR(
                            "Time: %f (s), still waiting got previous "
                            "one ...\n",
                            ((double)raw_timestamp() / 1000 - start_timestamp) /
                                1000);
                        show_log = 0;
                    }
                    pre_id_tmp = atoi(num);
                    if (pre_id != pre_id_tmp /*&& target_ratio!=OK_SIGNAL*/)
                    {
                        got_alpha   = true;
                        cnt_tmp     = 0;
                        pre_id      = pre_id_tmp;
                        target_cwnd = atof(alpha);
                        if (ESTIMATE)
                        {
                            double tmp_target_cwnd = ACCURACY * target_cwnd;
                            u64    alpha_tmp       = round(tmp_target_cwnd);
                            target_cwnd            = alpha_tmp / ACCURACY;
                        }

                        if (!TURN_ON_SAFETY)
                        {
                            if (ACCUMULATE_CWND)
                            {
                                cwnd_precise = mul(cwnd_precise, target_cwnd);
                                if (ROUNT_TYPE == FLOORING)
                                {
                                    cwnd_tmp = floor(cwnd_precise);
                                }
                                else if (ROUNT_TYPE == CEILING)
                                {
                                    cwnd_tmp = ceil(cwnd_precise);
                                }
                                else if (ROUNT_TYPE == ROUNDING)
                                {
                                    cwnd_tmp = round(cwnd_precise);
                                }
                                else
                                {
                                    cwnd_tmp = (u64)(cwnd_precise);
                                }
                                if (cwnd_tmp >= MAX_32Bit)
                                    target_ratio = MAX_32Bit;
                                else
                                    target_ratio = cwnd_tmp;
                            }
                            else
                            {
                                target_ratio = floor(mul(
                                    (double)sage_info.snd_cwnd, target_cwnd));
                            }
                        }

                        if (target_ratio < MIN_CWND)
                        {
                            target_ratio = MIN_CWND;
                            cwnd_precise = MIN_CWND;
                        }

                        if (target_ratio > MAX_CWND)
                        {
                            target_ratio = MAX_CWND;
                            cwnd_precise = MAX_CWND;
                        }

                        ret1 = setsockopt(sock_for_cnt[0], IPPROTO_TCP,
                                          TCP_CWND, &target_ratio,
                                          sizeof(target_ratio)); // 设置 cwnd
                        if (ret1 < 0)
                        {
                            DBGERROR("setsockopt: for index:%d ... %s (ret1:%d)\n",
                                     0, strerror(errno), ret1);
                            return ((void*)0);
                        }
                        cwnd_rate = target_cwnd;
                        error_cnt = 0;
                    }
                    else
                    {
                        if (error_cnt > 600000)
                        {
                            DBGERROR("After 1 Minute, stilll no new value "
                                     "id:%d prev_id:%d, Server is going "
                                     "down down down\n",
                                     pre_id_tmp, pre_id);
                            send_traffic = false;
                            error_cnt    = 0;
                        }
                        error_cnt++;
                        usleep(10);
                        if (error_cnt % 10000 == 0)
                            DBGERROR("---------Actor_id: %d, PreID: %d\n",
                                     actor_id, pre_id);
                    }

                    error2_cnt = 0;
                }
                else // 未读取到数据, 睡眠并等待
                {
                    usleep(10);
                    if (error2_cnt % 1000 == 0)
                        DBGERROR("NULL-ALPHA => cnt=%d! Actor_id: %d, PreID: %d num:%s alpha:%s\n",
                                 error2_cnt, actor_id, pre_id, num, alpha);
                    error2_cnt += 1;
                }
                if (!got_alpha &&
                    (timestamp() / 1000 - action_get_time_start_ms) >
                        WAIT_FOR_ACTION_MAX_ms)
                {
                    DBGERROR(
                        "............. Time: %f (s), No action from RL "
                        "agent (id:%d), skipping this round "
                        "................\n",
                        ((double)raw_timestamp() / 1000 - start_timestamp) /
                            1000,
                        pre_id);
                    // Skip this round!
                    got_alpha = 1;
                }
            }
        }
        else
        {
            target_ratio = 4;
            ret1         = setsockopt(sock_for_cnt[0], IPPROTO_TCP, TCP_CWND,
                              &target_ratio, sizeof(target_ratio));
            if (ret1 < 0)
            {
                DBGERROR("setsockopt: for index:%d ... %s (ret1:%d)\n",
                         0, strerror(errno), ret1);
                return ((void*)0);
            }
            usleep(report_period);
            t5                        = timestamp();
            double slient_per_is_done = (double)((t5 - t4) / 100000.0);
            // 100ms passed?
            if (slient_per_is_done > 1.0)
            {
                cc_state = state_DRL;
            }
        }
    }

    shmdt(shared_memory); // sage  -> actor
    shmctl(shmid, IPC_RMID, NULL);
    shmdt(shared_memory_rl); // actor -> sage
    shmctl(shmid_rl, IPC_RMID, NULL);
    free(shared_memory_rl2);

    return ((void*)0);
}

void* DataThread(void* info)
{
    cFlow* flow       = (cFlow*)info;
    int    sock_local = flow->flowinfo.sock;
    char*  src_ip;
    char   write_message[BUFSIZ + 1];
    char   read_message[1024] = {0};
    int    len;
    char*  savePtr;
    char*  dst_addr;
    u64    loop;
    u64    remaining_size;

    DBGERROR("the client address is %s:%d\n", inet_ntoa(flow->dst_addr.sin_addr), flow->dst_addr.sin_port);

    memset(write_message, 1, BUFSIZ);
    write_message[BUFSIZ] = '\0';
    /**
     * Get the RQ from client : {src_add} {flowid} {size} {dst_add}
     */
    len = recv(sock_local, read_message, 1024, 0);
    if (len <= 0)
    {
        DBGMARK(DBGSERVER, 1, "recv failed! \n");
        close(sock_local);
        return 0;
    }
    /**
     * For Now: we send the src IP in the RQ too!
     */
    src_ip = strtok_r(read_message, " ", &savePtr);
    if (src_ip == NULL)
    {
        // discard message:
        DBGMARK(DBGSERVER, 1, "id: %d discarding this message:%s \n",
                flow->flowinfo.flowid, savePtr);
        close(sock_local);
        return 0;
    }
    char* isstr = strtok_r(NULL, " ", &savePtr);
    if (isstr == NULL)
    {
        // discard message:
        DBGMARK(DBGSERVER, 1, "id: %d discarding this message:%s \n",
                flow->flowinfo.flowid, savePtr);
        close(sock_local);
        return 0;
    }
    flow->flowinfo.flowid = atoi(isstr);
    char* size_           = strtok_r(NULL, " ", &savePtr);
    flow->flowinfo.size   = 1024 * atoi(size_);
    DBGPRINT(DBGSERVER, 4, "%s\n", size_);
    dst_addr = strtok_r(NULL, " ", &savePtr);
    if (dst_addr == NULL)
    {
        // discard message:
        DBGMARK(DBGSERVER, 1, "id: %d discarding this message:%s \n",
                flow->flowinfo.flowid, savePtr);
        close(sock_local);
        return 0;
    }
    char* time_s_ = strtok_r(NULL, " ", &savePtr);
    char* endptr;
    start_of_client = strtoimax(time_s_, &endptr, 10);
    got_message     = 1;
    DBGPRINT(DBGSERVER, 2, "Got message: %" PRIu64 " us\n", timestamp());
    flow->flowinfo.rem_size = flow->flowinfo.size;
    DBGPRINT(DBGSERVER, 2, "time_rcv:%" PRIu64 " get:%s\n", start_of_client,
             time_s_);

    // Get detailed address
    strtok_r(src_ip, ".", &savePtr);
    if (dst_addr == NULL)
    {
        // discard message:
        DBGMARK(DBGSERVER, 1, "id: %d discarding this message:%s \n",
                flow->flowinfo.flowid, savePtr);
        close(sock_local);
        return 0;
    }

    // Calculate loops. In each loop, we can send BUFSIZ (8192) bytes of data
    loop = flow->flowinfo.size / BUFSIZ * 1024;
    // Calculate remaining size to be sent
    remaining_size = flow->flowinfo.size * 1024 - loop * BUFSIZ;
    send_begun = timestamp();

    while (send_traffic)
    {
        len = strlen(write_message);
        while (len > 0)
        {
            DBGMARK(DBGSERVER, 7, "++++++%d\n", send_traffic);
            len -= send(sock_local, write_message, strlen(write_message), 0);
            // usleep(50);
            usleep(1);
            DBGMARK(DBGSERVER, 7, "      ------\n");
        }
        // usleep(100);
        usleep(2);
    }
    flow->flowinfo.rem_size = 0;
    done                    = true;
    DBGPRINT(DBGSERVER, 1, "done=true\n");
    close(sock_local);
    DBGPRINT(DBGSERVER, 1, "done\n");
    return ((void*)0);
}

/* sage.cc:
 *  1. 算法针对 1 条流设计 (尽管设置了 flow_num 遍历, 但实际上限制了 flow_num 为 1, 应该是代码尚未完善)
 *  2. system() 在子线程中运行 tcpactor.py, 启动一个 actor
 *  3. 主线程等待 tcpactor.py 加载模型结束, 两个进程通过一个 shared_memory 通信
 *  4. system() 在新的子线程中创建 mahimahi 环境, 随后运行 client 程序, 创建一个主动发起连接的客户端
 *  5. 主线程等待 mahimahi 生成初始时间戳 (经过 patched 的 mahimahi 模拟器)
 *  6. 主进程绑定 ip:port 监听 (any:port)
 *  7. 通过 fdset 检查连接建立情况, 建立后生成三个子线程:
 *     DataThread (数据发送), CntThread (环境信息统计), TimerThread (计时停止发送)
 *  8. DataThread:  接受 client 发送的数据信息, 然后进入 while 循环, 不断向内核提供数据
 *  9. CntThread:   统计环境信息, 将环境信息写入 shared_memory, 等待 actor 的输出, 然后跳转 cwnd, 并写入内核
 * 10. TimerThread: 计时, 超时后修改 send_traffic 停止发送
 */

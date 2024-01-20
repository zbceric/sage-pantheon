# EVN
import numpy as np
import math
import sysv_ipc
import signal
import sys
from time import sleep
import dm_env
from dm_env import specs

class TCP_dmEnv_Wrapper(dm_env.Environment):
    def __init__(self, name='TCP', params=None, config=None, for_init_only=False, shrmem_r=None, shrmem_w=None, use_normalizer=False,id=0,num_flows=1,env_bw = 48):


        self.config = config
        self.params = params
        self.shrmem_r = shrmem_r
        self.shrmem_w = shrmem_w
        self.prev_rid = 99999
        self.wid = 23
        self.local_counter = 0
        self.pre_samples = 0.0
        self.new_samples = 0.0
        self.avg_delay = 0.0
        self.avg_thr = 0.0
        self.thr_ = 0.0
        self.del_ = 0.0
        self.max_bw = 0.0
        self.max_cwnd = 0.0
        self.max_smp = 0.0
        self.min_del = 9999999.0
        self.new_max = 1
        self.pre_loss = 0
        self.min_bw = 9999999
        self.min_rev_rtt = 0.5

        self.cnt_er =0

        self.pre_alpha = 0.0
        self._reset_next_step = False
        self.use_normalizer = use_normalizer
        self.id = id
        self.num_flows = num_flows
        self.env_bw = env_bw

        if self.use_normalizer == True:
            raise NotImplementedError('Normalizer is disabled')
        else:
            self.normalizer = None


    # 向共享内存写入 "99999 99999" 表示模型加载完毕
    def reset0(self):
        # start signal
        self.shrmem_w.write(str(99999) + " " + str(99999) + "\0")
        print("ID 99999-------- RL module is ready")

        self._reset_next_step = False
        state, delay_, rew0, error_code = self.get_state()
        
        self.reset_state = dm_env.restart(state)        # 重启环境
        return dm_env.restart(state), delay_, error_code

    def reset(self):
        
        return self.reset_state 

    # 获取状态, 从共享内存 mem_r 中读取一条数据, 将读取到的数据转换为 numpy 数组
    # 读取错误 or 没有读取到内容, sleep(self.config['poll_fq'])
    def get_state(self, evaluation=False):
        succeed = False
        error_cnt = 0
        
        while(1):
            # Read value from shared memory
            if error_cnt > 60000:
                
                error_cnt = 0
                sys.stderr.write("[Actor: "+str(self.id)+"] After 1 minute, We didn't get any state from the server. We are going down down down ...\n")
                sys.exit(0)

            try:
                memory_value = self.shrmem_r.read()

            except sysv_ipc.ExistentialError:
                print("No shared memory Now, python ends gracefully :)")
                
                sys.exit(0)

            memory_value = memory_value.decode('unicode_escape')

            
            # Find the 'end' of the string and strip
            i = memory_value.find('\0')                 # 找到停止符

            if i != -1:

                memory_value = memory_value[:i]         # 输入向量
                
                readstate = np.fromstring(memory_value, dtype=float, sep=' ')   # 分隔符是 ' '
                
                try:
                    rid = readstate[0]                  # msg_id
                except:
                    rid = self.prev_rid
                    error_cnt = error_cnt+1
                    sleep(self.config['poll_fq'])
                    continue
                try:
                    s0 = readstate[1:]                  # [1, 77] 77 维向量
                except:

                    error_cnt = error_cnt+1
                    sleep(self.config['poll_fq'])
                    continue


                if rid != self.prev_rid:                # msg_id 出现了变化, 意味着读取成功

                    succeed = True
                    break
                else:
                    wwwwww = ""
                    

            error_cnt = error_cnt+1

            sleep(self.config['poll_fq'])

        error_cnt = 0
        if succeed == False:
            raise ValueError('read Nothing new from shrmem for a long time')

        # input_dim: messages got from c code
        # obs_dim: actual dims pass to RL agent
        obs = np.zeros(self.config['obs_dim'],)     # 69, 目前还有 77 维, 其中 time_on_trace[0], max_tmp[1], [2 - 63] [64 - 74] reward[75], last_action [76]
        reward = 0  
        
        state = obs

        if len(s0) == (self.config['input_dim']):   # 输入向量的列数 0/1/4/5/6/8/64/75 不要
            self.prev_rid = rid                     # rid
            d = 0

            ## ======  State ====== ##
            # new mahimahi version's obs
            state = s0[self.config['obs_cols']]     # 状态
            

            ## ======  Reward ====== ##
            # new mahimahi version's default reward:
            reward = s0[-2]                         # 奖励值
            

            state, reward = self.check_values(state,reward)             # 检查 state 和 reward 是否合法
            return np.float32(state), d, np.float32(reward), True       # 转换为 np.float32 格式
        else:
            sys.stderr.write("s0: "+str(s0)+"\ns0.length: "+str(len(s0)))
            return np.float32(state), 0.0, np.float32(reward), False

    def check_values(self,o,r):
        for i in range(len(o)):
            if math.isnan(o[i]) or math.isinf(o[i]):
                with open('wrongvalues.txt', 'a') as f:
                    f.write("[Actor: "+str(self.id)+"] NONE/INF signal detected: index="+str(i)+" input: "+str(o))
                sys.stderr.write("[Actor: "+str(self.id)+"] NONE signal detected: index="+str(i)+" input: "+str(o))
                o[i]=0
        if math.isnan(r) or math.isinf(r):
            with open('wrongvalues.txt', 'a') as f:
                f.write("[Actor: "+str(self.id)+"] NONE/INF reward detected: index="+str(r))
            r = 0
        return o,r


    # 映射 action - 将 action 转换为 m_action
    def map_action(self,action):
        if self.config['action_version']==9:
            m_action = math.pow(2,round(action,3))
            
        else:
            
            m_action = action

        return m_action


    # 将 action 写入 shrmem_w
    def write_action(self, action):
        modified_action = self.map_action(action[0])

        msg = str(self.wid)+" "+str(modified_action)+"\0"

        self.shrmem_w.write(msg)
        self.wid = (self.wid + 1) % 1000

    # 转换时间步
    def _convert_timestep(self, ts):
        return ts._replace(discount=np.array(ts.discount, copy=False, dtype=np.float32))
    
    def step0(self, action, eval_=False):
        s1, delay_, rew0, error_code = self.get_state(evaluation=eval_)     # 获取状态

        #return s1, rew0, False, error_code
        if self._reset_next_step:
            self.step_state = self._convert_timestep(dm_env.termination(reward=rew0, observation=s1))
            return self._convert_timestep(dm_env.termination(reward=rew0, observation=s1)), delay_, error_code
            
        else:
            self.step_state = self._convert_timestep(dm_env.transition(reward=rew0, observation=s1))
            return self._convert_timestep(dm_env.transition(reward=rew0, observation=s1)), delay_, error_code
            

    def step(self, action):
        return self.step_state


    # 设置输入输出的规模以及最大最小值
    def observation_spec(self):
        return specs.BoundedArray(shape=(self.config['obs_dim'],), dtype='float32', name='observation',minimum=[-1e6], maximum=[1e6])

    def action_spec(self):
        if self.config['action_version']==9:

            return specs.BoundedArray(shape=(1,), dtype='float32', name='action', minimum=[-self.config['action_max']], maximum=[self.config['action_max']])
        else:
            return specs.BoundedArray(shape=(1,), dtype='float32', name='action', minimum=[0.], maximum=[1500.])


class Moving_Win():
    def __init__(self):
        print("Hello")


class Normalizer():

    def __init__(self):
        print("Hello")
        #raise NotImplementedError('Normalizer is disabled')

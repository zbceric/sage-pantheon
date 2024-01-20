
from sage_rl.offline.offline_agent import make_networks
import sysv_ipc
from envwapper import TCP_dmEnv_Wrapper
from acme.agents.tf import actors
import sys
from ruamel.yaml import YAML
from acme import wrappers
from acme.tf import utils as tf2_utils
from acme import specs
from acme.tf import networks
import sonnet as snt
import tensorflow as tf

import numpy as np
import os
os.environ['TF_CPP_MIN_LOG_LEVEL'] = '3'


# use cpu only for evaluation
os.environ["CUDA_VISIBLE_DEVICES"] = "-1"
tf.config.set_visible_devices([], 'GPU')

yaml = YAML()


def run_evalcrr(args):

    config_rl_module_path = os.path.dirname(os.path.realpath(__file__))     # 模型路径

    with open(os.path.join(config_rl_module_path, "config-rl-eval.yaml"), 'r') as fs:
        config = yaml.load(fs)

    try:
        shrmem_r = sysv_ipc.SharedMemory(args.mem_r)            # 共享内存
        shrmem_w = sysv_ipc.SharedMemory(args.mem_w)            # 共享内存
    except:
        shrmem_r = None
        shrmem_w = None
        sys.stderr.write("ISSUE WITH SHARED MEMORY ...\n")

        return

    # load env 加载环境
    env = TCP_dmEnv_Wrapper(config=config['tcpspec'], for_init_only=False, shrmem_r=shrmem_r,
                            shrmem_w=shrmem_w, id=args.id, env_bw=args.bw, num_flows=args.flows)

    env = wrappers.SinglePrecisionWrapper(env)
    environment_spec = specs.make_environment_spec(env)     # 输入输出, observation_spec / action_spec

    policy_network = None

    try:
        nw_type = config['nw_type']
    except:
        nw_type = config['offline_config']['type']      # 1909

    if nw_type in [1909] or config['offline_config']['use_offline_model']:
        networks_dict = make_networks(environment_spec.actions,
                                      vmin=config['offline_config']['vmin'], vmax=config['offline_config']['vmax'],
                                      num_atoms=config['offline_config']['num_atoms'],
                                      p_lstm_size=config['offline_config']['policy_lstm_size'],
                                      c_lstm_size=config['offline_config']['critic_lstm_size'],
                                      p_enc_size=config['offline_config']['p_enc_size'],
                                      c_enc_size=config['offline_config']['c_enc_size'],
                                      p_mlp_size=config['offline_config']['p_mlp_size'],
                                      c_mlp_size=config['offline_config']['c_mlp_size'],
                                      p_mlp_depth=config['offline_config']['p_mlp_depth'],
                                      c_mlp_depth=config['offline_config']['c_mlp_depth'],
                                      nw_type=config['offline_config']['type']
                                      )     # 创建网络
    else:
        raise ValueError("nw_type not supported")

    policy_network = networks_dict['policy']
    tf2_utils.create_variables(network=policy_network,
                               input_spec=[environment_spec.observations])      # 生成 策略网络 (actor)

    
    # evaluation 模式, 加载 policy 网络模型
    if config['offline_config']['use_offline_model']:
        if config['offline_config']['load_ckpt_dir'] is not None:
            load_path = config['offline_config']['load_ckpt_dir']
            load_path = os.path.normpath(
                os.path.join(config_rl_module_path, load_path))
            print("Loading model from ckpt_dir: ", load_path)
            assert tf.train.latest_checkpoint(
                load_path) != None, "no checkpoint is loaded, please choose the ckpt directory"
            checkpointer_rl = tf.train.Checkpoint(policy=policy_network)
            checkpointer_rl.restore(
                tf.train.latest_checkpoint(load_path)).expect_partial()
    else:
        raise ValueError("model not supported")

    assert policy_network is not None, "no policy model is chosen, nw_type or set config['offline_config']['use_offline_model']=true in config"

    eval_policy = snt.DeepRNN([
        policy_network,
        networks.StochasticMeanHead(),                      # 随机变量平均值
        networks.ClipToSpec(environment_spec.actions),      # 输出 action
    ])

    actor = actors.RecurrentActor(policy_network=eval_policy)

    iterations = np.int64(0)

    agent_actions_list = []

    

    dummy_observation = np.float32(np.ones((config['tcpspec']['obs_dim'],)))
    actor.select_action(dummy_observation)
    actor._state = actor._network.initial_state(1)
    actor._state = None

    try:
        timestep, _, _ = env.reset0()       # 重置环境, 第一个时间步
        timestep = env.reset()
        actor.observe_first(timestep)       # 第一次观察, 注意这里不需要是初始 timestep

    except Exception as e:
        print(e)
        print("================================ ERROR: TCP env has problem =============================")
        return

    # write 1st action
    a = actor.select_action(timestep.observation)   # 根据观察结果
    env.write_action(a)                             # 写入 action

    agent_actions_list.append(a)                    # 记录 action

    def should_not_stop(iterations):
        return True

    episode_return = 0
    actor_steps_taken = 0
    num_episodes = 0
    
    return_report_period = config['tcpactor']['return_report_period']

    while iterations < config['num_training_step']:     # 1000000
        iterations += 1        

        next_timestep, _, error_code = env.step0(a, eval_=args.eval)        # 获取一组观察数据, 注意, 该函数内部设有一个循环, 在没读取到数据时会睡眠 10ms, 然后再次尝试读取, 直到读取数据
        next_timestep = env.step(a)                     # 返回的是 step0 输出的 next_timestep, 本来应该是环境执行 a, 输出下一个时间步

        if error_code == True:

            actor.observe(action=a, next_timestep=next_timestep)            

            a1 = actor.select_action(next_timestep.observation)     # 选择合适的 action

            env.write_action(a1)                        # 写入 action

            episode_return += next_timestep.reward
            actor_steps_taken += 1

            

        else:
            sys.stderr.write("Invalid state received...\n")
            env.write_action(a)
            continue

        if iterations % return_report_period == 0:

            episode_return = 0
            num_episodes += 1

        a = a1

"""
dm_env.Environment: An abstract base class for RL environments.
dm_env.TimeStep:    A container class representing the outputs of the environment on each time step (transition).
                    环境在每个时间步的输出
dm_env.specs:       A module containing primitives that are used to describe the format of the actions consumed by 
                    an environment, as well as the observations, rewards, and discounts it returns.
                    输入输出的格式设置
"""
#!/usr/bin/env python

from os import path
from subprocess import check_call

import arg_parser
import context
from helpers import kernel_ctl
import sys
def setup_sage():
    sys.stderr.write("Before using Sage in Pantheon, make sure you have installed conda and the proper Kernel patches (4.19.112-0062)") 

def main():
    args = arg_parser.sender_first(sender_extend=None, receiver_extend=None)

    cc_repo1 = path.join(context.third_party_dir, 'sage-pantheon')
    cc_repo = path.join(cc_repo1,'sage_rl')
    rl_fld = path.join(cc_repo, 'rl_module')
    send_src = path.join(rl_fld, 'sage')
    recv_src = path.join(rl_fld, 'client')

    if args.option == 'setup':
        sh_path  = path.join(cc_repo, 'build.sh')
        mod_cmd = 'chmod +x ' + sh_path
        print(mod_cmd)
        check_call(mod_cmd, shell=True, cwd=cc_repo1)
        check_call(sh_path, shell=True, cwd=cc_repo1)
        sh_path = path.join(cc_repo, 'cp_models.sh')
        mod_cmd = 'chmod +x ' + sh_path
        check_call(mod_cmd, shell=True, cwd=cc_repo1)
        check_call(sh_path, shell=True, cwd=cc_repo1)
    
    if args.option == 'setup_after_reboot':
        setup_sage()
        return

    if args.option == 'sender':
        """ Please modify the cmd and write the absolute path of the Python interpreter
            and the absolute path of the Python library
            ./sage [env] [python interpreter] [port] [path to run.py] [scheme: pure] [actor_id]\n"
        """
        cmd = [send_src,
               "LD_LIBRARY_PATH=/home/zhangbochun/anaconda3/envs/sage/lib",
               "/home/zhangbochun/anaconda3/envs/sage/bin/python",
               args.port, rl_fld, "pure", "1"]
        check_call(cmd)
        return

    if args.option == 'receiver':
        # 输入: ip, port
        cmd = [recv_src, args.ip, "1" ,args.port]
        check_call(cmd)
        return

if __name__ == '__main__':
    main()

g++ -pthread src/sage.cc src/flow.cc src/define.cc -o sage
g++ src/client.c -o client

mv client sage rl_module/
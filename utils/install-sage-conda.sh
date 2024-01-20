#!/bin/bash

flag=false
envs=$(conda info --envs | awk '{print $1}')    # 获取 conda 全部环境名称
sage_envs="sage-pantheon"

while IFS= read -r line; do
    if [ "$line" == "$sage_envs" ]; then
        flag=true
    fi
done <<< "$envs"

if [ "$flag" == true ]; then
    echo "Sage environment already exists."
else
    echo "Sage environment not exists."
    conda create -n $sage_envs python=3.6 -y

    source activate $sage_envs
    pip install -r requirements.txt -i  https://pypi.tuna.tsinghua.edu.cn/simple
    pip install -e .
fi

# Sage-pantheon
Sage is a congestion control scheme based on offline reinforcement learning, which is introduced in [SIGCOMM'23 paper](https://dl.acm.org/doi/10.1145/3603269.3604838). Please refer to the GitHub repository ["Sage"](https://github.com/Soheil-ab/sage.git) for more information.

Sage-pantheon is designed to test Sage in [Pantheon](https://github.com/StanfordSNR/pantheon.git). The patched Linux kernel and the pre-trained Sage model in Sage-pantheon repository are copied from [Sage](https://github.com/Soheil-ab/sage.git).

## Prerequisites

### Adding Sage-pantheon repository to Pantheon
```bash
cd pantheon
git submodule add https://github.com/zbceric/sage-pantheon third_party/sage-pantheon
git submodule update --init --recursive
```

### Install the patched Linux Kernel
```bash
cd pantheon/third_party/sage-pantheon/linux-patch/
sudo dpkg -i linux-image-4.19.112-0062_4.19.112-0062-10.00.Custom_amd64.deb
sudo dpkg -i linux-headers-4.19.112-0062_4.19.112-0062-10.00.Custom_amd64.deb
sudo vim /etc/default/grub
# modify GRUB_DEFAULT:
>  GRUB_DEFAULT='Ubuntu, with Linux 4.19.112-0062'
sudo update-grub
sudo reboot 
```

### Verifying the new kernel
After installing the Sage's kernel and restarting your system, check if the system is using the new kernel:
```bash
uname -r
```
If the output isn't 4.19.112-0062, you should prioritize the installed Kernel in the grub list.

### Installing Python environment.
Require pre-installation of conda.
```bash
bash utils/install-sage-conda.sh
```

If conda is not installed, you can alternatively use venv or directly install the packages..
```bash
pip install -r requirements.txt -i  https://pypi.tuna.tsinghua.edu.cn/simple
pip install -e .
```

### Building Sage
```bash
cd pantheon
vim src/config.yml
> sage-pantheon:
>     name: Sage-pantheon
>     color: green
>     marker: '+'
cp third_party/sage-pantheon/sage-pantheon.py src/wrappers/
src/experiments/setup.py --setup --schemes "sage-pantheon"
```

## Testing Sage
To ensure that the Sage sender has sufficient time to load the neural network, set `run_first_setup_time` in `test.py` to a large value.
```python
self.run_first_setup_time = 20
```

Testing Sage in pantheon
```python
src/experiments/test.py local --scheme "sage-pantheon"
```


self.run_first_setup_time = 20

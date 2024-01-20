path=$(dirname $(readlink -f "$0"))
cd $path

target_dir=rl_module/sage_models/final
mkdir -p $target_dir
cp -v ../models/* $target_dir/
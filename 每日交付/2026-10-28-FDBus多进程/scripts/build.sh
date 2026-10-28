cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
set -o pipefail
cmake --build build --target ipc_log_system --parallel 1 2>&1 | tee /tmp/hjm-build.log
./build/local_log_demo --count 10
bash scripts/run_demo.sh build logs/linux-check 120
bash scripts/run_control_demo.sh build logs/control-check 120
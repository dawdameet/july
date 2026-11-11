TARGET ?= 192.168.1.41:8080

CC ?= clang
SYSROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)

CFLAGS_SERVER = -Wall -Wextra -O3 -lpthread
CFLAGS_DISPATCHER = -Wall -Wextra -O2

ifneq ($(SYSROOT),)
    CFLAGS_SERVER += -isysroot $(SYSROOT)
    CFLAGS_DISPATCHER += -isysroot $(SYSROOT)
endif

all: dispatcher dispatch server picture

dispatcher: dispatcher.c workflow.h task.h
	$(CC) $(CFLAGS_DISPATCHER) -o dispatcher dispatcher.c

dispatch: dispatcher
	ln -sf dispatcher dispatch

server: server.c workflow.h task.h
	$(CC) $(CFLAGS_SERVER) -o server server.c

picture: picture.c
	$(CC) $(CFLAGS_DISPATCHER) -o picture picture.c

# ---------------- Application Tests ----------------

test: test-sysbench

test-sysbench: dispatcher
	@echo "================================================================="
	@echo " [1/4] Dispatching Linux System Telemetry & Prime Benchmark"
	@echo " Target: $(TARGET)"
	@echo "================================================================="
	cd tests/01_system_benchmark && ../../dispatcher $(TARGET) workflow workflow.json Makefile src/main.c
	@echo "\n[*] Artifact Check:"
	@file tests/01_system_benchmark/sys_bench
	@echo "\n[*] Report:"
	@cat tests/01_system_benchmark/benchmark_report.json
	@echo "\n[✓] System Benchmark Passed!"

test-matrix: dispatcher
	@echo "================================================================="
	@echo " [2/4] Dispatching Matrix SIMD & Memory Cache Locality Test"
	@echo " Target: $(TARGET)"
	@echo "================================================================="
	cd tests/02_matrix_simd && ../../dispatcher $(TARGET) workflow workflow.json Makefile src/main.c
	@echo "\n[*] Artifact Check:"
	@file tests/02_matrix_simd/matrix_simd
	@file tests/02_matrix_simd/matrix_result.bin
	@echo "\n[*] Report:"
	@cat tests/02_matrix_simd/matrix_report.json
	@echo "\n[✓] Matrix SIMD Test Passed!"

test-kv: dispatcher
	@echo "================================================================="
	@echo " [3/4] Dispatching Concurrent In-Memory Key-Value & WAL Engine"
	@echo " Target: $(TARGET)"
	@echo "================================================================="
	cd tests/03_kv_storage && ../../dispatcher $(TARGET) workflow workflow.json Makefile src/main.c
	@echo "\n[*] Artifact Check:"
	@file tests/03_kv_storage/kv_engine
	@ls -lh tests/03_kv_storage/wal.bin
	@echo "\n[*] Report:"
	@cat tests/03_kv_storage/kv_report.json
	@echo "\n[✓] Key-Value Storage Test Passed!"

test-pipeline: dispatcher
	@echo "================================================================="
	@echo " [4/4] Dispatching Hybrid C Generator + Python Stream Analytics"
	@echo " Target: $(TARGET)"
	@echo "================================================================="
	cd tests/04_data_pipeline && ../../dispatcher $(TARGET) workflow workflow.json Makefile analyze.py src/main.c
	@echo "\n[*] Artifact Check:"
	@ls -lh tests/04_data_pipeline/ticks.bin
	@echo "\n[*] Report:"
	@cat tests/04_data_pipeline/analytics_summary.json
	@echo "\n[✓] Data Pipeline Test Passed!"

test-sandbox: dispatcher
	@echo "================================================================="
	@echo " [5/5] Dispatching Rich Manifest & Sandbox Isolation Test"
	@echo " Target: $(TARGET)"
	@echo "================================================================="
	cd tests/05_sandbox_rich && ../../dispatcher $(TARGET) workflow workflow.json Makefile src/main.c
	@echo "\n[*] Artifact Check:"
	@file tests/05_sandbox_rich/sandbox_probe
	@echo "\n[*] Report:"
	@cat tests/05_sandbox_rich/sandbox_report.json
	@echo "\n[✓] Sandbox Isolation Test Passed!"

test-sync: dispatcher
	@echo "================================================================="
	@echo " [6/6] Dispatching Persistent Project Workspace & Delta Sync"
	@echo " Target: $(TARGET)"
	@echo "================================================================="
	cd tests/06_codebase_sync && ../../dispatcher $(TARGET) build
	@echo "\n[*] Artifact Check:"
	@file tests/06_codebase_sync/sync_app
	@echo "\n[✓] Project Workspace & Delta Sync Test Passed!"

test-all: test-sysbench test-matrix test-kv test-pipeline test-sandbox test-sync
	@echo "\n================================================================="
	@echo "       [✓] ALL 6 REMOTE LINUX APPLICATIONS PASSED!               "
	@echo "================================================================="

clean:
	rm -f server dispatcher dispatch picture app
	rm -f tests/01_system_benchmark/sys_bench tests/01_system_benchmark/benchmark_report.json
	rm -f tests/02_matrix_simd/matrix_simd tests/02_matrix_simd/matrix_result.bin tests/02_matrix_simd/matrix_report.json
	rm -f tests/03_kv_storage/kv_engine tests/03_kv_storage/wal.bin tests/03_kv_storage/kv_report.json
	rm -f tests/04_data_pipeline/tick_gen tests/04_data_pipeline/ticks.bin tests/04_data_pipeline/analytics_summary.json
	rm -f tests/sys_bench tests/benchmark_report.json
	rm -f tests/06_codebase_sync/sync_app tests/06_codebase_sync/*.o
	rm -rf tests/06_codebase_sync/.july

.PHONY: all test test-sysbench test-matrix test-kv test-pipeline test-sandbox test-sync test-all clean

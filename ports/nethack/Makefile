PYTHON ?= python3
JOBS ?= 4

.NOTPARALLEL:

.PHONY: all bootstrap tools data tiles fuse-x fuzix fuse-x-nx fuzix-nx stage test
all: data fuse-x fuzix
bootstrap:
	$(PYTHON) bootstrap.py
tools: bootstrap
	$(PYTHON) build.py fuse-x --group makedefs -j $(JOBS)
	$(PYTHON) build.py fuse-x --group lev_comp -j $(JOBS)
	$(PYTHON) build.py fuse-x --group dgn_comp -j $(JOBS)
data: tools
	$(PYTHON) generate.py
fuse-x:
	$(PYTHON) build.py fuse-x -j $(JOBS)
fuzix:
	$(PYTHON) build.py fuzix -j $(JOBS)
tiles:
	$(PYTHON) tiles.py
fuse-x-nx:
	$(PYTHON) build.py fuse-x --frontend nx -j $(JOBS)
fuzix-nx:
	$(PYTHON) build.py fuzix --frontend nx -j $(JOBS)
stage:
	$(PYTHON) stage.py ../../fuse-x-shared
test:
	$(PYTHON) test_normalize.py
	$(PYTHON) test_io.py native
	$(PYTHON) test_io.py ripple
	$(PYTHON) test_io.py native --packed
	$(PYTHON) test_io.py ripple --packed
	$(PYTHON) test_io.py native --keys
	$(PYTHON) test_io.py ripple --keys
	$(PYTHON) test_runtime.py native --shared --home /home/tester --keys
	$(PYTHON) test_runtime.py ripple --keys
	$(PYTHON) test_runtime.py native --levels
	$(PYTHON) test_runtime.py ripple --levels

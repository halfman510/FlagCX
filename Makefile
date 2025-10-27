# 2025 - Modified by MetaX Integrated Circuits (Shanghai) Co., Ltd. All Rights Reserved.
# 2025 - Modified by DU. All Rights Reserved.
BUILDDIR ?= $(abspath ./build)
#已了解的内容：每个模块的作用
#未了解的内容：
#$(strip ...)、$(abspath ...) 等 make 的内置函数。
#-fPIC、-fvisibility=default 等 g++ 的编译选项。
#-Wl,--no-as-needed、-Wl,-rpath,... 等 g++ 的链接器选项。
#@mkdir -p \dirname $@`` 这种 shell 命令的具体写法。


# set to 0 if not provided
USE_NVIDIA ?= 0 #=表示如果这个变量还没有被定义，就设置为0
USE_ASCEND ?= 0
USE_ILUVATAR_COREX ?= 0
USE_CAMBRICON ?= 0
USE_GLOO ?= 0
USE_BOOTSTRAP ?= 0
USE_METAX ?= 0
USE_MUSA ?= 0
USE_KUNLUNXIN ?=0
USE_AMD ?= 0
USE_DU ?= 0
USE_MPI ?= 0
USE_UCX ?= 0

# set to empty if not provided
DEVICE_HOME ?=
CCL_HOME ?=
HOST_CCL_HOME ?=
MPI_HOME ?=
UCX_HOME ?=

ifeq ($(strip $(DEVICE_HOME)),) #如果用户安装了，但没有手动指定DEVICE_HOME，自动猜测最常见的安装路径
	ifeq ($(USE_NVIDIA), 1)
		DEVICE_HOME = /usr/local/cuda
	else ifeq ($(USE_ASCEND), 1)
		DEVICE_HOME = /usr/local/Ascend/ascend-toolkit/latest
	else ifeq ($(USE_ILUVATAR_COREX), 1)
		DEVICE_HOME = /usr/local/corex
	else ifeq ($(USE_CAMBRICON), 1)
		DEVICE_HOME = $(NEUWARE_HOME)
	else ifeq ($(USE_METAX), 1)
		DEVICE_HOME = /opt/maca
	else ifeq ($(USE_MUSA), 1)
		DEVICE_HOME = /usr/local/musa
	else ifeq ($(USE_KUNLUNXIN), 1)
		DEVICE_HOME = /usr/local/xpu
	else ifeq ($(USE_DU), 1)
		DEVICE_HOME = ${CUDA_PATH}
	else ifeq ($(USE_AMD), 1)
		DEVICE_HOME = /opt/rocm
	else
		DEVICE_HOME = /usr/local/cuda
	endif
endif

ifeq ($(strip $(CCL_HOME)),) #设备端集体通信库
	ifeq ($(USE_NVIDIA), 1)
		CCL_HOME = /usr/local/nccl/build
	else ifeq ($(USE_ASCEND), 1)
		CCL_HOME = /usr/local/Ascend/ascend-toolkit/latest
	else ifeq ($(USE_ILUVATAR_COREX), 1)
		CCL_HOME = /usr/local/corex
	else ifeq ($(USE_CAMBRICON), 1)
		CCL_HOME = $(NEUWARE_HOME)
	else ifeq ($(USE_METAX), 1)
		CCL_HOME = /opt/maca
	else ifeq ($(USE_MUSA), 1)
		CCL_HOME = /usr/local/musa
	else ifeq ($(USE_KUNLUNXIN), 1)
		CCL_HOME = /usr/local/xccl
	else ifeq ($(USE_DU), 1)
		CCL_HOME = ${CUDA_PATH}
	else ifeq ($(USE_AMD), 1)
		CCL_HOME = /opt/rocm
	else
		CCL_HOME = /usr/local/nccl/build
	endif
endif

ifeq ($(strip $(HOST_CCL_HOME)),) #主机通信库
	ifeq ($(USE_GLOO), 1)
		HOST_CCL_HOME = /usr/local
	else ifeq ($(USE_MPI), 1)
		HOST_CCL_HOME = $(MPI_HOME)
	else
		HOST_CCL_HOME = 
	endif
endif

ifeq ($(strip $(MPI_HOME)),)
	ifeq ($(USE_MPI), 1)
		MPI_HOME = /usr/local
	endif
endif

#无论底层是哪种高速网络硬件（比如 InfiniBand、RoCE、Ethernet），它都能为上层应用提供一套统一的、极致性能的数据传输接口。
ifeq ($(strip $(UCX_HOME)),) #UCX 的全称是 Unified Communication X，开源的、高性能的底层通信框架。
	ifeq ($(USE_UCX), 1)
		UCX_HOME = /usr/local/ucx
	endif
endif
#---------------------------------上面都是一样的功能

DEVICE_LIB =
DEVICE_INCLUDE =
DEVICE_LINK =
CCL_LIB =
CCL_INCLUDE =
CCL_LINK =
HOST_CCL_LIB = 
HOST_CCL_INCLUDE =
HOST_CCL_LINK =
ADAPTOR_FLAG =
HOST_CCL_ADAPTOR_FLAG =
UCX_LIB =
UCX_INCLUDE =
UCX_LINK =
NET_ADAPTOR_FLAG =
ifeq ($(USE_NVIDIA), 1) #根据启用的USE开关，设置了真正在g++命令里的参数；LIB库文件搜索路径、INCLUDE头文件搜索路径、LINK链接的具体库名、ADAPTOR_FLAG宏相关
	DEVICE_LIB = $(DEVICE_HOME)/lib64
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	DEVICE_LINK = -lcudart -lcuda
	CCL_LIB = $(CCL_HOME)/lib
	CCL_INCLUDE = $(CCL_HOME)/include
	CCL_LINK = -lnccl
	ADAPTOR_FLAG = -DUSE_NVIDIA_ADAPTOR
else ifeq ($(USE_ASCEND), 1)
	DEVICE_LIB = $(DEVICE_HOME)/lib64
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	DEVICE_LINK = -lascendcl
	CCL_LIB = $(CCL_HOME)/lib64
	CCL_INCLUDE = $(CCL_HOME)/include
	CCL_LINK = -lhccl
	ADAPTOR_FLAG = -DUSE_ASCEND_ADAPTOR
else ifeq ($(USE_ILUVATAR_COREX), 1)
	DEVICE_LIB = $(DEVICE_HOME)/lib
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	DEVICE_LINK = -lcudart -lcuda
	CCL_LIB = $(CCL_HOME)/lib
	CCL_INCLUDE = $(CCL_HOME)/include
	CCL_LINK = -lnccl
	ADAPTOR_FLAG = -DUSE_ILUVATAR_COREX_ADAPTOR
else ifeq ($(USE_CAMBRICON), 1)
	DEVICE_LIB = $(DEVICE_HOME)/lib64
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	DEVICE_LINK = -lcnrt
	CCL_LIB = $(CCL_HOME)/lib64
	CCL_INCLUDE = $(CCL_HOME)/include
	CCL_LINK = -lcncl
	ADAPTOR_FLAG = -DUSE_CAMBRICON_ADAPTOR
else ifeq ($(USE_METAX), 1)
	DEVICE_LIB = $(DEVICE_HOME)/lib64
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	CCL_LIB = $(CCL_HOME)/lib64
	CCL_INCLUDE = $(CCL_HOME)/include
	CCL_LINK = -lmccl
	ADAPTOR_FLAG = -DUSE_METAX_ADAPTOR
else ifeq ($(USE_MUSA), 1)
	DEVICE_LIB = $(DEVICE_HOME)/lib
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	CCL_LIB = $(CCL_HOME)/lib
	CCL_INCLUDE = $(CCL_HOME)/include
	CCL_LINK = -lmccl -lmusa
	ADAPTOR_FLAG = -DUSE_MUSA_ADAPTOR
else ifeq ($(USE_KUNLUNXIN), 1)
	DEVICE_LIB = $(DEVICE_HOME)/so
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	DEVICE_LINK = -lxpurt -lcudart
	CCL_LIB = $(CCL_HOME)/so
	CCL_INCLUDE = $(CCL_HOME)/include
	CCL_LINK = -lbkcl
	ADAPTOR_FLAG = -DUSE_KUNLUNXIN_ADAPTOR
else ifeq ($(USE_DU), 1)
	DEVICE_LIB = $(DEVICE_HOME)/lib64
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	DEVICE_LINK = -lcudart -lcuda
	CCL_LIB = $(CCL_HOME)/lib64
	CCL_INCLUDE = $(CCL_HOME)/include
	CCL_LINK = -lnccl
	ADAPTOR_FLAG = -DUSE_DU_ADAPTOR
else ifeq ($(USE_AMD), 1)
	DEVICE_LIB = $(DEVICE_HOME)/lib
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	DEVICE_LINK = -lhiprtc
	CCL_LIB = $(CCL_HOME)/lib
	CCL_INCLUDE = $(CCL_HOME)/include/rccl
	CCL_LINK = -lrccl
	ADAPTOR_FLAG = -DUSE_AMD_ADAPTOR -D__HIP_PLATFORM_AMD__
else
	DEVICE_LIB = $(DEVICE_HOME)/lib64
	DEVICE_INCLUDE = $(DEVICE_HOME)/include
	DEVICE_LINK = -lcudart -lcuda
	CCL_LIB = $(CCL_HOME)/lib
	CCL_INCLUDE = $(CCL_HOME)/include
	CCL_LINK = -lnccl
	ADAPTOR_FLAG = -DUSE_NVIDIA_ADAPTOR
endif

ifeq ($(USE_GLOO), 1)
	HOST_CCL_LIB = $(HOST_CCL_HOME)/lib
	HOST_CCL_INCLUDE = $(HOST_CCL_HOME)/include
	HOST_CCL_LINK = -lgloo
	HOST_CCL_ADAPTOR_FLAG = -DUSE_GLOO_ADAPTOR
else ifeq ($(USE_MPI), 1)
	HOST_CCL_LIB = $(MPI_HOME)/lib
	HOST_CCL_INCLUDE = $(MPI_HOME)/include
	HOST_CCL_LINK = -lmpi
	HOST_CCL_ADAPTOR_FLAG = -DUSE_MPI_ADAPTOR
else ifeq ($(USE_BOOTSTRAP), 1)
	HOST_CCL_LIB = /usr/local/lib
	HOST_CCL_INCLUDE = /usr/local/include
	HOST_CCL_LINK = 
	HOST_CCL_ADAPTOR_FLAG = -DUSE_BOOTSTRAP_ADAPTOR
else
	HOST_CCL_LIB = /usr/local/lib
	HOST_CCL_INCLUDE = /usr/local/include
	HOST_CCL_LINK = 
	HOST_CCL_ADAPTOR_FLAG = -DUSE_BOOTSTRAP_ADAPTOR
endif

# UCX network adaptor configuration
ifeq ($(USE_UCX), 1)
	UCX_LIB = $(UCX_HOME)/lib
	UCX_INCLUDE = $(UCX_HOME)/include
	UCX_LINK = -lucp -lucs -luct
	NET_ADAPTOR_FLAG = -DUSE_UCX
else
	UCX_LIB = $(UCX_HOME)/lib
	UCX_INCLUDE = $(UCX_HOME)/include
	UCX_LINK = 
	NET_ADAPTOR_FLAG = 
endif

#定义了输出目录(build/lib, build/obj)，BUILDDIR汇总了所有需要查找的头文件目录
LIBDIR := $(BUILDDIR)/lib
OBJDIR := $(BUILDDIR)/obj

INCLUDEDIR := \
	$(abspath flagcx/include) \
	$(abspath flagcx/core) \
	$(abspath flagcx/adaptor) \
	$(abspath flagcx/adaptor/include) \
	$(abspath flagcx/service)

#LIBSRCFILES: 使用 wildcard 函数自动查找所有 .cc 源文件，非常灵活，增删代码文件时无需修改 Makefile
LIBSRCFILES:= \
	$(wildcard flagcx/*.cc) \
	$(wildcard flagcx/core/*.cc) \
	$(wildcard flagcx/adaptor/*.cc) \
	$(wildcard flagcx/adaptor/device/*.cc) \
	$(wildcard flagcx/adaptor/ccl/*.cc) \
	$(wildcard flagcx/adaptor/net/*.cc) \
	$(wildcard flagcx/service/*.cc)

#LIBOBJ: 将所有源文件列表 (.cc) 转换成对应的目标文件列表 (.o)，并指定它们将被放在 OBJDIR 目录下。
LIBOBJ     := $(LIBSRCFILES:%.cc=$(OBJDIR)/%.o)

#all: 是默认目标，当我们输入 make 时，它就会尝试生成 libflagcx.so。
#定义了如何将所有编译好的目标文件 (.o) 链接成一个最终的动态库文件 (.so)。这里用到了第3步中设置的所有 -L 和 -l 参数。
TARGET = libflagcx.so
all: $(LIBDIR)/$(TARGET)

print_var:
	@echo "USE_KUNLUNXIN : $(USE_KUNLUNXIN)"
	@echo "DEVICE_HOME: $(DEVICE_HOME)"
	@echo "CCL_HOME: $(CCL_HOME)"
	@echo "HOST_CCL_HOME: $(HOST_CCL_HOME)"
	@echo "MPI_HOME: $(MPI_HOME)"
	@echo "USE_NVIDIA: $(USE_NVIDIA)"
	@echo "USE_ASCEND: $(USE_ASCEND)"
	@echo "USE_ILUVATAR_COREX: $(USE_ILUVATAR_COREX)"
	@echo "USE_CAMBRICON: $(USE_CAMBRICON)"
	@echo "USE_KUNLUNXIN: $(USE_KUNLUNXIN)"
	@echo "USE_GLOO: $(USE_GLOO)"
	@echo "USE_MPI: $(USE_MPI)"
	@echo "USE_MUSA: $(USE_MUSA)"
	@echo "USE_DU: $(USE_DU)"
	@echo "USE_AMD: $(USE_AMD)"
	@echo "DEVICE_LIB: $(DEVICE_LIB)"
	@echo "DEVICE_INCLUDE: $(DEVICE_INCLUDE)"
	@echo "CCL_LIB: $(CCL_LIB)"
	@echo "CCL_INCLUDE: $(CCL_INCLUDE)"
	@echo "HOST_CCL_LIB: $(HOST_CCL_LIB)"
	@echo "HOST_CCL_INCLUDE: $(HOST_CCL_INCLUDE)"
	@echo "ADAPTOR_FLAG: $(ADAPTOR_FLAG)"
	@echo "HOST_CCL_ADAPTOR_FLAG: $(HOST_CCL_ADAPTOR_FLAG)"
	@echo "USE_UCX: $(USE_UCX)"
	@echo "UCX_HOME: $(UCX_HOME)"
	@echo "UCX_LIB: $(UCX_LIB)"
	@echo "UCX_INCLUDE: $(UCX_INCLUDE)"
	@echo "NET_ADAPTOR_FLAG: $(NET_ADAPTOR_FLAG)"

#链接规则，定义了如何将任何一个 .cc 源文件编译成一个 .o 目标文件。
#$< 代表依赖的源文件 (.cc)。
#$@ 代表生成的目标文件 (.o)。
#这里用到了第3步中设置的所有 -I (头文件路径) 和最重要的 -D (ADAPTOR_FLAG) 参数。
#-c 表示只编译不链接，-fPIC 是生成动态库所必需的位置无关代码选项。
$(LIBDIR)/$(TARGET): $(LIBOBJ)
	@mkdir -p `dirname $@`
	@echo "Linking   $@"
	@g++ $(LIBOBJ) -o $@ -L$(CCL_LIB) -L$(DEVICE_LIB) -L$(HOST_CCL_LIB) -L$(UCX_LIB) -shared -fvisibility=default -Wl,--no-as-needed -Wl,-rpath,$(LIBDIR) -Wl,-rpath,$(CCL_LIB) -Wl,-rpath,$(HOST_CCL_LIB) -Wl,-rpath,$(UCX_LIB) -lpthread -lrt -ldl $(CCL_LINK) $(DEVICE_LINK) $(HOST_CCL_LINK) $(UCX_LINK) -g

$(OBJDIR)/%.o: %.cc
	@mkdir -p `dirname $@`
	@echo "Compiling $@"
	@g++ $< -o $@ $(foreach dir,$(INCLUDEDIR),-I$(dir)) -I$(CCL_INCLUDE) -I$(DEVICE_INCLUDE) -I$(HOST_CCL_INCLUDE) -I$(UCX_INCLUDE) $(ADAPTOR_FLAG) $(HOST_CCL_ADAPTOR_FLAG) $(NET_ADAPTOR_FLAG) -c -fPIC -fvisibility=default -Wvla -Wno-unused-function -Wno-sign-compare -Wall -MMD -MP -g

-include $(LIBOBJ:.o=.d)

clean:
	@rm -rf $(LIBDIR)/$(TARGET) $(OBJDIR)
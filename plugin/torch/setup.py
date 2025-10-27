import os
import sys
from setuptools import setup
# Disable auto load flagcx when setup
os.environ["TORCH_DEVICE_BACKEND_AUTOLOAD"] = "0"
from torch.utils import cpp_extension
from setuptools import setup, find_packages
#已了解的内容：每个模块的作用
#未了解的内容：具体的参数名称

# 解析命令行参数
adaptor_flag = "-DUSE_NVIDIA_ADAPTOR"
if '--adaptor' in sys.argv:# 检查运行它的时候有没有提供--adaptor参数，比如 python setup.py develop --adaptor klx
    arg_index = sys.argv.index('--adaptor')
    sys.argv.remove("--adaptor")
    if arg_index < len(sys.argv):# 类似makefile，根据提供的平台名称，生成对应的C++宏定义（如 -DUSE_KUNLUNXIN_ADAPTOR）
        assert sys.argv[arg_index] in ["nvidia", "iluvatar_corex", "cambricon", "metax", "du", "klx", "ascend", "musa", "amd"], f"Invalid adaptor: {adaptor_flag}"
        print(f"Using {sys.argv[arg_index]} adaptor")
        if sys.argv[arg_index] == "iluvatar_corex":
            adaptor_flag = "-DUSE_ILUVATAR_COREX_ADAPTOR"
        elif sys.argv[arg_index] == "cambricon":
            adaptor_flag = "-DUSE_CAMBRICON_ADAPTOR"
        elif sys.argv[arg_index] == "metax":
            adaptor_flag = "-DUSE_METAX_ADAPTOR"
        elif sys.argv[arg_index] == "musa":
            adaptor_flag = "-DUSE_MUSA_ADAPTOR"
        elif sys.argv[arg_index] == "du":
            adaptor_flag = "-DUSE_DU_ADAPTOR"
        elif sys.argv[arg_index] == "klx":
            adaptor_flag = "-DUSE_KUNLUNXIN_ADAPTOR"
        elif sys.argv[arg_index] == "ascend":
            adaptor_flag = "-DUSE_ASCEND_ADAPTOR"
        elif sys.argv[arg_index] == "amd":
            adaptor_flag = "-DUSE_AMD_ADAPTOR"
    else:
        print("No adaptor provided after '--adaptor'. Using default nvidia adaptor")
    sys.argv.remove(sys.argv[arg_index])# 这个宏会传递给 C++ 编译器，确保“转接头” C++ 代码也是用匹配硬件平台的模式来编译的

#定义“转接头”的C++源代码和依赖
sources = ["flagcx/src/backend_flagcx.cpp", "flagcx/src/utils_flagcx.cpp"]# 定义了构成这个“转接头”本身需要编译的 C++ 文件。注意，这里只有两个胶水代码文件，而不是整个 FlagCX 项目。
include_dirs = [
    f"{os.path.dirname(os.path.abspath(__file__))}/flagcx/include",
    f"{os.path.dirname(os.path.abspath(__file__))}/../../flagcx/include",
]# 告诉编译器去哪里找头文件，包括 FlagCX 核心库的头文件 (../../flagcx/include)

library_dirs = [
    f"{os.path.dirname(os.path.abspath(__file__))}/../../build/lib",
]# 告诉链接器去哪里找我们之前用 Makefile 编译好的 libflagcx.so 文件。../../build/lib 这个相对路径指的就是项目根目录下的 build/lib 文件夹。

libs = ["flagcx"]# 明确告诉链接器，需要链接到 libflagcx.so 这个库。

# 添加特定平台的额外依赖
if adaptor_flag == "-DUSE_NVIDIA_ADAPTOR":# 根据第1步中确定的 adaptor_flag，为这个“转接头”添加上它所需要链接的其他系统库
    include_dirs += ["/usr/local/cuda/include"]
    library_dirs += ["/usr/local/cuda/lib64"]
    libs += ["cuda", "cudart", "c10_cuda", "torch_cuda"]# 如果为了英伟达编译，PyTorch拓展要链接libflagcx.so，还有CUDA 的运行时库、PyTorch 的 CUDA 库等，这样它才能处理 PyTorch 传过来的 GPU 张量 (Tensor)。
elif adaptor_flag == "-DUSE_ILUVATAR_COREX_ADAPTOR":
    include_dirs += ["/usr/local/corex/include"]
    library_dirs += ["/usr/local/corex/lib64"]
    libs += ["cuda", "cudart", "c10_cuda", "torch_cuda"]
elif adaptor_flag == "-DUSE_CAMBRICON_ADAPTOR":
    import torch_mlu
    neuware_home_path=os.getenv("NEUWARE_HOME")
    pytorch_home_path=os.getenv("PYTORCH_HOME")
    torch_mlu_path = torch_mlu.__file__.split("__init__")[0]
    torch_mlu_lib_dir = os.path.join(torch_mlu_path, "csrc/lib/")
    torch_mlu_include_dir = os.path.join(torch_mlu_path, "csrc/")
    include_dirs += [f"{neuware_home_path}/include", torch_mlu_include_dir]
    library_dirs += [f"{neuware_home_path}/lib64", torch_mlu_lib_dir]
    libs += ["cnrt", "cncl", "torch_mlu"]
elif adaptor_flag == "-DUSE_METAX_ADAPTOR":
    include_dirs += ["/opt/maca/include"]
    library_dirs += ["/opt/maca/lib64"]
    libs += ["cuda", "cudart", "c10_cuda", "torch_cuda"]
elif adaptor_flag == "-DUSE_MUSA_ADAPTOR":
    include_dirs += ["/usr/local/musa/include"]
    library_dirs += ["/usr/local/musa/lib64"]
    libs += ["musa", "mudart", "c10_musa", "torch_musa"]
elif adaptor_flag == "-DUSE_DU_ADAPTOR":
    include_dirs += ["${CUDA_PATH}/include"]
    library_dirs += ["${CUDA_PATH}/lib64"]
    libs += ["cuda", "cudart", "c10_cuda", "torch_cuda"]
elif adaptor_flag == "-DUSE_KUNLUNXIN_ADAPTOR":
    include_dirs += ["/opt/kunlun/include"]
    library_dirs += ["/opt/kunlun/lib"]
    libs += ["cuda", "cudart", "c10_cuda", "torch_cuda"]
elif adaptor_flag == "-DUSE_ASCEND_ADAPTOR":
    import torch_npu# NPU (Neural-network Processing Unit)，即神经网络处理单元，昇腾芯片在硬件层面就是为AI计算深度定制的专用处理器（NPU）；有不同于 GPU 的架构。因此，在软件层面，它也需要一套完全不同的、专门适配 NPU 的库。
    pytorch_npu_install_path = os.path.dirname(os.path.abspath(torch_npu.__file__))
    pytorch_library_path = os.path.join(pytorch_npu_install_path, "lib")
    include_dirs += [os.path.join(pytorch_npu_install_path, "include")]
    library_dirs += [pytorch_library_path]
    libs += ["torch_npu"]
elif adaptor_flag == "-DUSE_AMD_ADAPTOR":
    include_dirs += ["/opt/rocm/include"]
    library_dirs += ["/opt/rocm/lib"]
    libs += ["hiprtc", "c10_hip", "torch_hip"]
# 定义并配置 C++ 扩展模块（打包）
# 这是 PyTorch 提供的标准工具。它把前面我们准备的所有“材料”（源文件、头文件路径、库文件路径、要链接的库）都打包在一起。
module = cpp_extension.CppExtension(
    name='flagcx._C',#定义了编译出来的 C++ 扩展在 Python 中叫什么名字
    sources=sources,
    include_dirs=include_dirs,
    extra_compile_args={
        'cxx': [adaptor_flag]
    },#最关键的一步！ 在这里，它把第1步中生成的 -DUSE_..._ADAPTOR 宏，正式地传递给了 C++ 编译器。
    extra_link_args=["-Wl,-rpath,"+f"{os.path.dirname(os.path.abspath(__file__))}/../../build/lib"],
    library_dirs=library_dirs,
    libraries=libs,
)

#执行打包和安装
setup(
    name="flagcx",
    version="0.1.0",
    ext_modules=[module],#告诉 setuptools，我们这个 Python 包包含一个需要编译的 C++ 扩展。
    cmdclass={'build_ext': cpp_extension.BuildExtension},#指定使用 PyTorch 提供的 BuildExtension 来执行编译，它知道如何处理 CUDA/C++ 相关的复杂编译任务。
    packages=find_packages(),
    entry_points={"torch.backends": ["flagcx = flagcx:init"]},#利用 Python 包的“入口点”机制，向 PyTorch “注册” 自己。“嗨，PyTorch！我这里有一个叫 flagcx 的后端，如果你需要初始化它，请调用 flagcx 包里的 init 函数。”
)

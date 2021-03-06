#include <thread>

#if ((defined _M_IX86 || defined __i386__) || (defined _M_X64 || defined __x86_64__))
#ifndef _WIN32
#include <cpuid.h>
#else
#include <intrin.h>
#endif
#endif

#ifdef PPCOPT
#ifdef HAVE_AUXV
#include <sys/auxv.h>
#endif
#endif

#include <limits.h>
#include <string.h>

#include "w2xconv.h"
#include "Buffer.hpp"
#include "modelHandler.hpp"
#include "convertRoutine.hpp"
#include "filters.hpp"
#include "cvwrap.hpp"
#ifdef _WIN32
   #include <windows.h>
#else
   #include <pthread.h>
#endif

namespace w2xc
{

static std::vector<struct W2XConvProcessor> processor_list;

static void global_init2(void)
{
	{
		W2XConvProcessor host;
		host.type = W2XCONV_PROC_HOST;
		host.sub_type = W2XCONV_PROC_HOST_OPENCV;
		host.dev_id = 0;
		host.dev_name = "Generic";
		host.num_core = std::thread::hardware_concurrency();

#if ((defined _M_IX86 || defined __i386__) || (defined _M_X64 || defined __x86_64__))
#ifdef _WIN32
#define x_cpuid(p,eax) __cpuid(p, eax)
		typedef int cpuid_t;
#else
#define x_cpuid(p,eax) __get_cpuid(eax, &(p)[0], &(p)[1], &(p)[2], &(p)[3]);
		typedef unsigned int cpuid_t;
#endif
		cpuid_t v[4];
		cpuid_t data[4*3+1];

		x_cpuid(v, 0x80000000);
		if ((unsigned int)v[0] >= 0x80000004)
		{
			x_cpuid(data+4*0, 0x80000002);
			x_cpuid(data+4*1, 0x80000003);
			x_cpuid(data+4*2, 0x80000004);
			data[12] = 0;

			host.dev_name = strdup((char*)data);
		}
		else
		{
			x_cpuid(data, 0x0);
			data[4] = 0;
			host.dev_name = strdup((char*)(data + 1));
		}

		x_cpuid(v, 1);

		if ((v[2] & 0x18000000) == 0x18000000)
		{
			if (v[2] & (1<<12))
			{
				host.sub_type = W2XCONV_PROC_HOST_FMA;
			}
			else
			{
				host.sub_type = W2XCONV_PROC_HOST_AVX;
			}
		}
		else if (v[2] & (1<<0))
		{
			host.sub_type = W2XCONV_PROC_HOST_SSE3;
		}
#endif

#if (defined _M_ARM || defined __arm__) || (defined _M_ARM64 || defined __aarch64__)
		host.dev_name = "ARM NEON";
		host.sub_type = W2XCONV_PROC_HOST_NEON;
#endif // ARMOPT

#ifdef PPCOPT
		bool have_altivec = false;
#ifdef HAVE_AUXV
		unsigned long cap = getauxval(AT_HWCAP);
		if (cap & (1U << 28)) /* HWCAP_ALTIVEC */
		{
			have_altivec = true;
		}
#else
		/* Assume that the machine has AltiVec
		 * since it passed the compile-time check. */
		have_altivec = true;
#endif // HAVE_AUXV
		if (have_altivec)
		{
			host.dev_name = "PowerPC AltiVec";
			host.sub_type = W2XCONV_PROC_HOST_ALTIVEC;
		}
#endif // PPCOPT

		processor_list.push_back(host);
	}

#ifdef CLLIB_H
	w2xc::initOpenCLGlobal(&processor_list);
#endif
#ifdef HAVE_CUDA
	w2xc::initCUDAGlobal(&processor_list);
#endif


	/*
	 * <priority>
	 * 1: NV CUDA
	 * 2: OCL GPU
	 * 3: host AVX
	 * 4: OCL GPU (intel gen)
	 * 5: host
	 * 6: other
	 *
	 * && orderd by num_core
	 */
	std::sort(
		processor_list.begin(),
		processor_list.end(),
		[&](W2XConvProcessor const &p0,W2XConvProcessor const &p1)
		{
			bool p0_is_opencl_gpu = (p0.type == W2XCONV_PROC_OPENCL) && ((p0.sub_type&W2XCONV_PROC_OPENCL_DEVICE_MASK) == W2XCONV_PROC_OPENCL_DEVICE_GPU);

			bool p1_is_opencl_gpu = (p1.type == W2XCONV_PROC_OPENCL) && ((p1.sub_type&W2XCONV_PROC_OPENCL_DEVICE_MASK) == W2XCONV_PROC_OPENCL_DEVICE_GPU);


			bool p0_is_opencl_intel_gpu = (p0.type == W2XCONV_PROC_OPENCL) && (p0.sub_type == W2XCONV_PROC_OPENCL_INTEL_GPU);

			bool p1_is_opencl_intel_gpu = (p1.type == W2XCONV_PROC_OPENCL) && (p1.sub_type == W2XCONV_PROC_OPENCL_INTEL_GPU);

			bool p0_host_avx = (p0.type == W2XCONV_PROC_HOST) && (p0.sub_type >= W2XCONV_PROC_HOST_AVX);

			bool p1_host_avx = (p1.type == W2XCONV_PROC_HOST) && (p1.sub_type >= W2XCONV_PROC_HOST_AVX);

			if (p0.type == p1.type)
			{
				if (p0.type == W2XCONV_PROC_OPENCL)
				{
					if (p0.sub_type != p1.sub_type)
					{
						if (p0_is_opencl_gpu)
						{
							return true;
						}

						if (p1_is_opencl_gpu)
						{
							return false;
						}
					}
				}

				if (p0.num_core != p1.num_core)
				{
					return p0.num_core > p1.num_core;
				}
			}
			else
			{
				if (p0.type == W2XCONV_PROC_CUDA)
				{
					return true;
				}

				if (p1.type == W2XCONV_PROC_CUDA)
				{
					return false;
				}

				if (p0_is_opencl_intel_gpu)
				{
					if (p1_host_avx)
					{
					  return false;
					}
				}

				if (p1_is_opencl_intel_gpu)
				{
					if (p0_host_avx)
					{
					  return false;
					}
				}

				if (p0_is_opencl_gpu)
				{
				return true;
				}

				if (p1_is_opencl_gpu)
				{
					return false;
				}
			}

			if (p0.type == W2XCONV_PROC_HOST)
			{
				return true;
			}

			if (p1.type == W2XCONV_PROC_HOST)
			{
				return false;
			}

			/* ?? */
			return p0.dev_id < p1.dev_id;
		}
	);
}

#ifdef _WIN32
static INIT_ONCE global_init_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK global_init1(PINIT_ONCE initOnce, PVOID Parameter, PVOID *Context)
{
	global_init2();
	return TRUE;
}

static void global_init(void) 
{
	InitOnceExecuteOnce(&global_init_once, global_init1, nullptr, nullptr);
}
#else
static pthread_once_t global_init_once = PTHREAD_ONCE_INIT;

static void global_init1()
{
	global_init2();
}

static void global_init()
{
	pthread_once(&global_init_once, global_init1);
}

#endif


const struct W2XConvProcessor * w2xconv_get_processor_list(size_t *ret_num)
{
	global_init();

	*ret_num = processor_list.size();
	return &processor_list[0];
}

static int select_device(enum W2XConvGPUMode gpu)
{
	size_t n = processor_list.size();
	if (gpu == W2XCONV_GPU_FORCE_OPENCL)
	{
		for (size_t i=0; i<n; i++)
		{
			if (processor_list[i].type == W2XCONV_PROC_OPENCL)
			{
				return (int) i;
			}
		}
	}

	int host_proc = 0;
	for (int i=0; i<n; i++)
	{
		if (processor_list[i].type == W2XCONV_PROC_HOST)
		{
			host_proc = i;
			break;
		}
	}

	if (gpu == W2XCONV_GPU_AUTO)
	{
		/* 1. CUDA
		 * 2. AMD GPU OpenCL
		 * 3. FMA
		 * 4. AVX
		 * 5. Intel GPU OpenCL
		 */

		for (int i=0; i<n; i++)
		{
			if (processor_list[i].type == W2XCONV_PROC_CUDA)
			{
				return i;
			}
		}

		for (int i=0; i<n; i++)
		{
			if ((processor_list[i].type == W2XCONV_PROC_OPENCL) && (processor_list[i].sub_type == W2XCONV_PROC_OPENCL_AMD_GPU))
			{
				return i;
			}
		}

		if (processor_list[host_proc].sub_type == W2XCONV_PROC_HOST_FMA || processor_list[host_proc].sub_type == W2XCONV_PROC_HOST_AVX)
		{
			return host_proc;
		}

		for (int i=0; i<n; i++)
		{
			if ((processor_list[i].type == W2XCONV_PROC_OPENCL) && (processor_list[i].sub_type == W2XCONV_PROC_OPENCL_INTEL_GPU))
			{
				return i;
			}
		}

		return host_proc;
	}

	/* (gpu == GPU_DISABLE) */
	for (int i=0; i<n; i++)
	{
		if (processor_list[i].type == W2XCONV_PROC_HOST)
		{
			return i;
		}
	}

	return 0; // ??
}

W2XConv* w2xconv_init()
{
	global_init();

   int processor_idx = select_device(W2XCONV_GPU_DISABLE);

	struct W2XConv *c = new struct W2XConv;
	struct W2XConvProcessor *proc = &processor_list[processor_idx];

	switch (proc->type)
	{
#ifdef HAVE_CUDA
		case W2XCONV_PROC_CUDA:
		{
			w2xc::initCUDA(&c->env, proc->dev_id);
		}break;
#endif
#ifdef CLLIB_H
		case W2XCONV_PROC_OPENCL:
		{
			if(!w2xc::initOpenCL(c, &c->env, proc))return NULL;
		}break;
#endif
	}

	c->target_processor = proc;
	c->last_error.code = W2XCONV_NOERROR;

	return c;
}

void clearError(W2XConv *conv)
{
	switch (conv->last_error.code)
	{
		case W2XCONV_NOERROR:
		case W2XCONV_ERROR_Y_MODEL_MISMATCH_TO_RGB_F32:
		case W2XCONV_ERROR_WIN32_ERROR:
		case W2XCONV_ERROR_LIBC_ERROR:
		case W2XCONV_ERROR_RGB_MODEL_MISMATCH_TO_Y:
		{
			break;
		}
		case W2XCONV_ERROR_WIN32_ERROR_PATH:
		{
			free(conv->last_error.u.win32_path.path);
			break;
		}
		case W2XCONV_ERROR_LIBC_ERROR_PATH:
		{
			free(conv->last_error.u.libc_path.path);
			break;
		}
		case W2XCONV_ERROR_MODEL_LOAD_FAILED:
		case W2XCONV_ERROR_IMREAD_FAILED:
		case W2XCONV_ERROR_IMWRITE_FAILED:
		{
			free(conv->last_error.u.path);
			break;
		}
		default:
		{
			break;
		}
	}
}

static void setError(W2XConv *conv, enum W2XConvErrorCode code)
{
	clearError(conv);
	conv->last_error.code = code;
}

void w2xconv_fini(struct W2XConv *conv)
{
	clearError(conv);

#ifdef HAVE_CUDA
	w2xc::finiCUDA(&conv->env);
#endif
#ifdef CLLIB_H
	w2xc::finiOpenCL(&conv->env);
#endif

	delete conv;
}

bool w2xconv_2x_rgb_f32_esenthel(struct W2XConv *conv, unsigned char *data, size_t pitch, int w, int h, bool clamp)
{
	W2Mat mat(w, h, CV_32FC3, data, pitch);
   return w2xc::convertWithModels(conv, &conv->env, mat, mat, conv->scale2_models, 0, w2xc::IMAGE_RGB_F32, clamp);
}

}
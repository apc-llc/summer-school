#ifndef DATA_H
#define DATA_H

// define some helper types that can be used to pass simulation
// data around without haveing to pass individual parameters
struct discretization_t
{
	int nx;       // x dimension
	int ny;       // y dimension
	int nt;       // number of time steps
	int N;        // total number of grid points
	double dt;    // time step size
	double dx;    // distance between grid points
	double alpha; // dx^2/(D*dt)
};

namespace cpu
{
	extern struct discretization_t options;

	extern cudaDeviceProp props;
}

namespace gpu
{
	// fields that hold the solution
	extern __device__ double * __restrict__ x_old; // 2d
	extern __device__ double * __restrict__ bndN, * __restrict__ bndE, * __restrict__ bndS, * __restrict__ bndW; // 1d

	extern __constant__ struct discretization_t options;
}

#include <thrust/extrema.h>

namespace gpu
{
	// We redefine dim3 under namespace, because the default one has
	// constructors, which is not allowed for types device variables
	// (dim3 is used as device vars type below to keep kernel compute
	// grid configuration).
	struct dim3
	{
		unsigned int x, y, z;
		
		__device__ operator ::dim3()
		{
			return ::dim3(x, y, z);
		}
	};
	
	// Use Thrust occupancy calculator to determine the best size of block.
	template<typename T>
	inline size_t get_optimal_szblock(T kernel)
	{
		using namespace gpu;
		using namespace thrust::system::cuda::detail;

		struct function_attributes_t attrs;
		{
			cudaFuncAttributes funcAttrs;
			CUDA_ERR_CHECK(cudaFuncGetAttributes(&funcAttrs, kernel));
			attrs.constSizeBytes = funcAttrs.constSizeBytes;
			attrs.localSizeBytes = funcAttrs.localSizeBytes;
			attrs.maxThreadsPerBlock = funcAttrs.maxThreadsPerBlock;
			attrs.numRegs = funcAttrs.numRegs;
			attrs.sharedSizeBytes = funcAttrs.sharedSizeBytes;
		}
		struct device_properties_t props;
		{
			props.major = cpu::props.major;
			memcpy(&props.maxGridSize, &cpu::props.maxGridSize, sizeof(int) * 3);
			props.maxThreadsPerBlock = cpu::props.maxThreadsPerBlock;
			props.maxThreadsPerMultiProcessor = cpu::props.maxThreadsPerMultiProcessor;
			props.minor = cpu::props.minor;
			props.multiProcessorCount = cpu::props.multiProcessorCount;
			props.regsPerBlock = cpu::props.regsPerBlock;
			props.sharedMemPerBlock = cpu::props.sharedMemPerBlock;
			props.warpSize = cpu::props.warpSize;
		}
		return block_size_with_maximum_potential_occupancy(attrs, props);
	}

	struct block_size_to_dynamic_smem_size : public thrust::unary_function<size_t, size_t>
	{
		float operator()(size_t szblock) { return szblock * sizeof(double); }
	};

	// Use Thrust occupancy calculator to determine the best size of block
	// for a kernel which uses dynamic shared memory
	template<typename T, typename block_size_to_dynamic_smem_size>
	inline size_t get_optimal_szblock(T kernel)
	{
		using namespace gpu;
		using namespace thrust::system::cuda::detail;

		struct function_attributes_t attrs;
		{
			cudaFuncAttributes funcAttrs;
			CUDA_ERR_CHECK(cudaFuncGetAttributes(&funcAttrs, kernel));
			attrs.constSizeBytes = funcAttrs.constSizeBytes;
			attrs.localSizeBytes = funcAttrs.localSizeBytes;
			attrs.maxThreadsPerBlock = funcAttrs.maxThreadsPerBlock;
			attrs.numRegs = funcAttrs.numRegs;
			attrs.sharedSizeBytes = funcAttrs.sharedSizeBytes;
		}
		struct device_properties_t props;
		{
			props.major = cpu::props.major;
			memcpy(&props.maxGridSize, &cpu::props.maxGridSize, sizeof(int) * 3);
			props.maxThreadsPerBlock = cpu::props.maxThreadsPerBlock;
			props.maxThreadsPerMultiProcessor = cpu::props.maxThreadsPerMultiProcessor;
			props.minor = cpu::props.minor;
			props.multiProcessorCount = cpu::props.multiProcessorCount;
			props.regsPerBlock = cpu::props.regsPerBlock;
			props.sharedMemPerBlock = cpu::props.sharedMemPerBlock;
			props.warpSize = cpu::props.warpSize;
		}
		return block_size_with_maximum_potential_occupancy(attrs, props, block_size_to_dynamic_smem_size());
	}

	template<typename T>
	inline void get_optimal_grid_block_config(T kernel,
		int nx, int ny, size_t szblock, dim3* grid, dim3* blocks)
	{
		grid->x = 1; grid->y = 1; grid->z = 1;
		blocks->x = 1; blocks->y = 1; blocks->z = 1;

		if (szblock > nx)
		{
			blocks->x = nx;
			blocks->y = min(ny, (int)szblock / blocks->x);
			grid->y = ny / blocks->y;
			if (ny % blocks->y) grid->y++;
		}
		else
		{
			blocks->x = szblock;
			grid->x = nx / blocks->x;
			if (nx % blocks->x) grid->x++;
			grid->y = ny;
		}
	}

	#define determine_optimal_grid_block_config(kernel_name, nx, ny) \
	{ \
		{ \
			using namespace gpu::kernel_name##_kernel; \
			gpu::config_t c; \
			size_t szblock = gpu::get_optimal_szblock(kernel); \
			gpu::get_optimal_grid_block_config(kernel, nx, ny, szblock, &c.grid, &c.block); \
			CUDA_ERR_CHECK(cudaMemcpyToSymbol(config, &c, sizeof(gpu::config_t))); \
		} \
	}

	#define determine_optimal_grid_block_config_reduction(kernel_name, nx, c, i) \
	{ \
		{ \
			using namespace gpu::kernel_name##_kernel; \
			size_t szblock = gpu::get_optimal_szblock(kernel); \
			gpu::get_optimal_grid_block_config(kernel, nx, 1, szblock, &c.grid, &c.block); \
			CUDA_ERR_CHECK(cudaMemcpyToSymbol(configs, &c, sizeof(gpu::config_t), i * sizeof(gpu::config_t))); \
		} \
	}

	#define MAX_CONFIGS 4

	#define determine_optimal_grid_block_configs_reduction(kernel_name, n) \
	{ \
		int length = n; \
		using namespace gpu; \
		int iconfig = 0; \
		config_t config; \
		determine_optimal_grid_block_config_reduction(kernel_name, length / 2 + length % 2, config, iconfig); \
		iconfig++; \
		for (int szbuffer = config.grid.x ; szbuffer != 1; szbuffer = config.grid.x) \
		{ \
			length = szbuffer / 2 + szbuffer % 2; \
			determine_optimal_grid_block_config_reduction(kernel_name, length / 2 + length % 2, config, iconfig); \
			iconfig++; \
		} \
	}

	template<typename T>
	inline T get_value(T& var)
	{
		T* ptr;
		CUDA_ERR_CHECK(cudaGetSymbolAddress((void**)&ptr, var));
		T value;
		CUDA_ERR_CHECK(cudaMemcpy(&value, ptr, sizeof(T), cudaMemcpyDeviceToHost));
		return value;
	}

	typedef struct __attribute__((packed)) { dim3 grid, block; } config_t;

	// round up to the power of 2
	template<typename T>
	inline __device__ T roundPow2(T ptr, int pow2)
	{
		size_t number = (size_t)ptr;
		pow2--;
		pow2 = 0x01 << pow2;
		pow2--;
		number--;
		number = number | pow2;
		number++;
		return (T)number;
	}
}

#endif // DATA_H


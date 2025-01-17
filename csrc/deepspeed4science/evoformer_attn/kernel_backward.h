/***************************************************************************************************
 * Copyright (c) 2017 - 2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holdvr nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 **************************************************************************************************/

// Copyright (c) Microsoft Corporation.
// SPDX-License-Identifier: Apache-2.0

// DeepSpeed Team

#pragma once

#include <cmath>
#include <type_traits>
#include <vector>

#include <cuda_fp16.h>

#include "cutlass/cutlass.h"
#include "cutlass/epilogue/thread/linear_combination.h"
#include "cutlass/epilogue/thread/scale_type.h"
#include "cutlass/fast_math.h"
#include "cutlass/functional.h"
#include "cutlass/gemm/gemm.h"
#include "cutlass/layout/matrix.h"
#include "cutlass/layout/vector.h"
#include "cutlass/numeric_conversion.h"
#include "cutlass/numeric_types.h"
#include "cutlass/tensor_ref.h"

#include "gemm_kernel_utils.h"

#include "cutlass/epilogue/thread/linear_combination_relu.h"
#include "cutlass/epilogue/threadblock/epilogue_smem_accumulator.h"
#include "cutlass/epilogue/warp/fragment_iterator_tensor_op.h"
#include "cutlass/epilogue/warp/tile_iterator_tensor_op.h"
#include "cutlass/gemm/device/default_gemm_configuration.h"
#include "cutlass/gemm/kernel/default_gemm.h"
#include "cutlass/gemm/threadblock/default_mma.h"
#include "cutlass/gemm/threadblock/default_mma_core_simt.h"
#include "cutlass/gemm/threadblock/default_mma_core_sm70.h"
#include "cutlass/gemm/threadblock/default_mma_core_sm75.h"
#include "cutlass/gemm/threadblock/default_mma_core_sm80.h"
#include "cutlass/matrix_shape.h"
#include "cutlass/platform/platform.h"
#include "cutlass/transform/threadblock/predicated_tile_iterator.h"
#include "cutlass/transform/threadblock/vector_iterator.h"
#include "epilogue/epilogue_pipelined.h"
#include "iterators/epilogue_predicated_tile_iterator.h"

#include "epilogue/epilogue_grad_bias.h"
#include "gemm/custom_mma.h"
#include "gemm/find_default_mma.h"
#include "gemm/mma_accum_lambda_iterator.h"
#include "gemm/mma_from_smem.h"
#include "transform/bias_broadcast.h"
#include "transform/tile_smem_loader.h"

#include <inttypes.h>

using namespace gemm_kernel_utils;

namespace {

template <typename FragmentType, int32_t kNumThreads>
struct GmemTile {
    /*
      Helper functions to efficient store/load RF to gmem

      GEMM accumulators have a particular format on A100, and
      it takes some compute/shared-memory to rearrange them to
      a RowMajor or ColumnMajor format in global memory through
      an Epilogue. The same complexity goes for loading into RF.

      This class loads/stores RF as they are, and can be used for
      efficient accumulation across gemms for instance:

      ```
      GmemTile tile;
      for (int i = 0; i < N; ++i) {
        // ...

        Fragment accum;
        if (i == 0) {
          accum.clear();
        } else {
          tile.load(accum);
        }
        mma(accum, ...);
        if (i < N-1) {
          // Store for next GEMM
          tile.store(accum);
        } else {
          // Store in tensor (eg RowMajor)
          epilogue(accum);
        }

        // ...
      }
      ```
    */

    // 128bits per thread
    using AccessType = cutlass::Array<float, 4>;
    static constexpr int32_t kBytes = sizeof(AccessType);
    static constexpr int32_t kStride = kNumThreads * AccessType::kElements;
    static constexpr int32_t kNumIters = FragmentType::kElements / AccessType::kElements;
    static constexpr int32_t kElementsStored = kNumThreads * FragmentType::kElements;
    static_assert(FragmentType::kElements % AccessType::kElements == 0,
                  "fragment not aligned on 128 bits");

    float* ptr;

    CUTLASS_DEVICE void load(FragmentType& fragment, int thread_id)
    {
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < kNumIters; ++i) {
            AccessType* __restrict__ gmem_ptr = reinterpret_cast<AccessType*>(
                ptr + thread_id * AccessType::kElements + i * kStride);
            AccessType sub_fragment;
            cutlass::arch::global_load<AccessType, kBytes>(sub_fragment, gmem_ptr, true);
            CUTLASS_PRAGMA_UNROLL
            for (int j = 0; j < AccessType::kElements; ++j) {
                fragment[i * AccessType::kElements + j] = sub_fragment[j];
            }
        }
    }

    CUTLASS_DEVICE void store(FragmentType const& fragment, int thread_id)
    {
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < kNumIters; ++i) {
            AccessType* __restrict__ gmem_ptr = reinterpret_cast<AccessType*>(
                ptr + thread_id * AccessType::kElements + i * kStride);
            AccessType sub_fragment;
            CUTLASS_PRAGMA_UNROLL
            for (int j = 0; j < AccessType::kElements; ++j) {
                sub_fragment[j] = fragment[i * AccessType::kElements + j];
            }
            cutlass::arch::global_store<AccessType, kBytes>(sub_fragment, gmem_ptr, true);
        }
    }
};

template <typename scalar_t, typename Arch>
constexpr int getWarpsPerSm()
{
    constexpr bool is_half = !cutlass::platform::is_same<scalar_t, float>::value;
    if (Arch::kMinComputeCapability >= 80) { return is_half ? 12 : 8; }
    return 8;
}
}  // namespace

template <
    // which arch we target (eg `cutlass::arch::Sm80`)
    typename ArchTag_,
    // input/output type
    typename scalar_t_,
    // run optimized kernel because memory accesses will be aligned
    bool kIsAligned_,
    // use dropout if enabled
    bool kApplyDropout_,
    // when doing a GEMM, preload the next one (uses more shmem)
    bool kPreload_,
    // block dimensions
    int kBlockSizeI_,
    int kBlockSizeJ_,
    // upperbound on `max(value.shape[-1], query.shape[-1])`
    int kMaxK_ = (int)cutlass::platform::numeric_limits<uint32_t>::max(),
    template <typename, typename, typename> class Broadcast1_ = BroadcastNoLoad,
    template <typename, typename, typename> class Broadcast2_ = BroadcastNoLoad>
struct AttentionBackwardKernel {
    using scalar_t = scalar_t_;
    using output_t = scalar_t;
    using output_accum_t = float;
    using lse_scalar_t = float;
    using accum_t = float;
    using ArchTag = ArchTag_;
    static constexpr bool kIsAligned = kIsAligned_;
    static constexpr bool kApplyDropout = kApplyDropout_;
    static constexpr bool kPreload = kPreload_;
    static constexpr int kBlockSizeI = kBlockSizeI_;
    static constexpr int kBlockSizeJ = kBlockSizeJ_;
    static constexpr int kMaxK = kMaxK_;

    struct Params {
        // Input tensors
        scalar_t* query_ptr;          // [Mq, nH, K]
        scalar_t* key_ptr;            // [Mk, nH, K]
        scalar_t* value_ptr;          // [Mk, nH, Kv]
        lse_scalar_t* logsumexp_ptr;  // [nH, Mq]
        scalar_t* output_ptr;         // [Mq, nH, Kv]
        scalar_t* grad_output_ptr;    // [Mq, nH, Kv]
        accum_t* delta_ptr;           // [nH, Mq]
        int32_t* cu_seqlens_q_ptr = nullptr;
        int32_t* cu_seqlens_k_ptr = nullptr;

        // Output tensors
        output_t* grad_query_ptr;  //  [Mq, nH, K]
        output_t* grad_key_ptr;    //    [Mk, nH, K]
        output_t* grad_value_ptr;  //  [Mk, nH, Kv]

        accum_t* grad_bias1_ptr = nullptr;
        accum_t* grad_bias2_ptr = nullptr;
        int32_t B = 0;
        int32_t N = 0;
        scalar_t* bias1_ptr = nullptr;
        scalar_t* bias2_ptr = nullptr;

        // Accumulators
        union {
            output_accum_t* workspace = nullptr;  // [Mq, Kq] + [Mkv, Kq] + [Mkv, Kv]
            output_accum_t* workspace_gk;
        };
        output_accum_t* workspace_gv;  // (will be calculated by the kernel)
        output_accum_t* workspace_gq;  // (will be calculated by the kernel)

        // Scale
        accum_t scale;

        // Dimensions/strides
        int32_t head_dim = -1;
        int32_t head_dim_value = -1;
        int32_t num_queries = -1;
        int32_t num_keys = -1;
        int32_t num_heads = -1;

        int32_t q_strideM;
        int32_t k_strideM;
        int32_t v_strideM;
        int32_t gO_strideM;
        int32_t gB_strideM;
        int8_t gQKV_strideM_multiplier = 1;  // 3 for packed, 1 otherwise

        // RNG sequence offset based on batch_id and head_id
        unsigned long long dropout_batch_head_rng_offset;
        float dropout_prob = 0.0f;

        CUTLASS_HOST_DEVICE int32_t o_strideM() const { return head_dim_value * num_heads; }
        CUTLASS_HOST_DEVICE int32_t gQ_strideM() const
        {
            return gQKV_strideM_multiplier * num_heads * head_dim;
        }
        CUTLASS_HOST_DEVICE int32_t gK_strideM() const
        {
            return gQKV_strideM_multiplier * num_heads * head_dim;
        }
        CUTLASS_HOST_DEVICE int32_t gV_strideM() const
        {
            return gQKV_strideM_multiplier * num_heads * head_dim_value;
        }

        // Everything below is only used in `advance_to_block`
        // and shouldn't use registers
        int64_t o_strideH;
        int32_t q_strideH;
        int32_t k_strideH;
        int32_t v_strideH;
        int64_t o_strideB;
        int64_t q_strideB;
        int64_t k_strideB;
        int64_t v_strideB;
        int64_t lse_strideB;
        int64_t lse_strideH;
        int64_t delta_strideB;
        int64_t delta_strideH;
        int32_t num_batches;

        int64_t gO_strideB = 0;
        int64_t gQ_strideB = 0;
        int64_t gK_strideB = 0;
        int64_t gV_strideB = 0;
        int64_t gB_strideB = 0;
        int64_t gO_strideH = 0;
        int64_t gQ_strideH = 0;
        int64_t gK_strideH = 0;
        int64_t gV_strideH = 0;
        int64_t gB_strideH = 0;

        CUTLASS_DEVICE bool advance_to_block()
        {
            int64_t batch_id = blockIdx.z;
            int32_t head_id = blockIdx.y;

            if (kNeedsAccumGradQ || kNeedsAccumGradK || kNeedsAccumGradV) {
                assert(workspace_size() == 0 || workspace != nullptr);

                workspace += (batch_id * num_heads + head_id) * workspace_strideBH();
                workspace = warp_uniform(workspace);
                workspace_gv = workspace + workspace_elements_gk();
                workspace_gq = workspace_gv + workspace_elements_gv();
            } else {
                workspace = nullptr;
            }

            // Advance pointers that depend on the total concatenated
            // number of queries, as `num_queries` is modified in the block
            // below
            dropout_batch_head_rng_offset = batch_id * (num_heads * num_queries * num_keys) +
                                            head_id * (num_queries * num_keys);
            logsumexp_ptr += batch_id * lse_strideB + head_id * lse_strideH;

            query_ptr += batch_id * q_strideB + head_id * q_strideH;
            key_ptr += batch_id * k_strideB + head_id * k_strideH;
            value_ptr += batch_id * v_strideB + head_id * v_strideH;
            output_ptr += batch_id * o_strideB + head_id * o_strideH;
            grad_output_ptr += batch_id * gO_strideB + head_id * gO_strideH;
            delta_ptr += batch_id * delta_strideB + head_id * delta_strideH;

            grad_query_ptr += batch_id * gQ_strideB + head_id * gQ_strideH;
            grad_key_ptr += batch_id * gK_strideB + head_id * gK_strideH;
            grad_value_ptr += batch_id * gV_strideB + head_id * gV_strideH;
            using broadcast_1 = Broadcast1_<typename MatmulQK::BiasLoader::ThreadMap,
                                            typename MatmulQK::BiasLoader::Shape,
                                            scalar_t>;
            using broadcast_2 = Broadcast2_<typename MatmulQK::BiasLoader::ThreadMap,
                                            typename MatmulQK::BiasLoader::Shape,
                                            scalar_t>;

            if (broadcast_1::kEnable && grad_bias1_ptr) {
                grad_bias1_ptr += batch_id * num_queries;
            }
            if (broadcast_2::kEnable && grad_bias2_ptr) {
                auto strideB = num_heads * num_queries * num_keys;
                auto strideH = num_queries * num_keys;
                grad_bias2_ptr += (batch_id / N) * strideB + head_id * strideH;
            }
            if (broadcast_1::kEnable && bias1_ptr) {
                bias1_ptr = broadcast_1::advance(bias1_ptr,
                                                 batch_id / N,
                                                 batch_id % N,
                                                 head_id,
                                                 num_queries * N,
                                                 num_queries,
                                                 0);
            }
            if (broadcast_2::kEnable && bias2_ptr) {
                auto strideB = num_heads * num_queries * num_keys;
                auto strideH = num_queries * num_keys;
                bias2_ptr = broadcast_2::advance(
                    bias2_ptr, batch_id / N, batch_id % N, head_id, strideB, 0, strideH);
            }

            num_queries = warp_uniform(num_queries);
            num_keys = warp_uniform(num_keys);

            query_ptr = warp_uniform(query_ptr);
            key_ptr = warp_uniform(key_ptr);
            value_ptr = warp_uniform(value_ptr);
            logsumexp_ptr = warp_uniform(logsumexp_ptr);
            output_ptr = warp_uniform(output_ptr);
            grad_output_ptr = warp_uniform(grad_output_ptr);
            delta_ptr = warp_uniform(delta_ptr);

            grad_query_ptr = warp_uniform(grad_query_ptr);
            grad_key_ptr = warp_uniform(grad_key_ptr);
            grad_value_ptr = warp_uniform(grad_value_ptr);
            if (broadcast_1::kEnable) {
                grad_bias1_ptr = warp_uniform(grad_bias1_ptr);
                bias1_ptr = warp_uniform(bias1_ptr);
            }
            if (broadcast_2::kEnable) {
                grad_bias2_ptr = warp_uniform(grad_bias2_ptr);
                bias2_ptr = warp_uniform(bias2_ptr);
            }

            return true;
        }

        __host__ dim3 getBlocksGrid() const { return dim3(1, num_heads, num_batches); }
        __host__ dim3 getThreadsGrid() const { return dim3(kWarpSize * kNumWarpsPerBlock, 1, 1); }
        CUTLASS_HOST_DEVICE int64_t workspace_elements_gk() const
        {
            if (!kNeedsAccumGradK) { return 0; }
            return align_up(num_keys, (int32_t)kBlockSizeJ) *
                   align_up(head_dim, (int32_t)kBlockSizeI);
        }
        CUTLASS_HOST_DEVICE int64_t workspace_elements_gv() const
        {
            if (!kNeedsAccumGradV) { return 0; }
            return align_up(num_keys, (int32_t)kBlockSizeJ) *
                   align_up(head_dim_value, (int32_t)kBlockSizeI);
        }
        CUTLASS_HOST_DEVICE int64_t workspace_elements_gq() const
        {
            if (!kNeedsAccumGradQ) { return 0; }
            if (num_keys <= kBlockSizeJ) { return 0; }
            return align_up(num_queries, (int32_t)kBlockSizeI) *
                   align_up(head_dim, (int32_t)kBlockSizeJ);
        }
        CUTLASS_HOST_DEVICE int64_t workspace_strideBH() const
        {
            // Aligned on 128bits
            return align_up(
                workspace_elements_gk() + workspace_elements_gv() + workspace_elements_gq(),
                int64_t(4));
        }
        CUTLASS_HOST_DEVICE int64_t workspace_size() const
        {
            // Returns size of buffer we need to run this kernel
            return num_batches * num_heads * workspace_strideBH() * sizeof(float);
        }
    };

    static constexpr int64_t kWarpSize = 32;

    // If this is true, we store and accumulate dK/dV in RF
    // rather than going back to gmem every time
    static constexpr bool kIsHalf = cutlass::sizeof_bits<scalar_t>::value <= 16;
    static constexpr bool kOutputInRF = kIsHalf && kMaxK <= kBlockSizeI;
    static_assert(!kPreload || (kIsHalf && ArchTag::kMinComputeCapability >= 80 && kOutputInRF),
                  "preload MMA not supported");
    static constexpr bool kPrologueQK = kPreload;
    static constexpr bool kPrologueGV = kPreload;
    static constexpr bool kPrologueDOV = kPreload;
    static constexpr bool kPrologueGQ = kPreload;
    static constexpr bool kPrologueGK = kPreload;

    static constexpr int64_t kNumWarpsPerBlock = (kBlockSizeI * kBlockSizeJ) / (32 * 32);

    // Compute delta for the f16 kernels
    // TODO: Figure out why it's slower on the f32 kernels
    // (something due to RF pressure?)
    // TODO: Remove condition on `kOutputInRF` - this is needed to work
    // around a compiler bug on V100, not exactly sure why but I spent
    // too much time on this already. Reproducible with
    // (B, Mq, Mkv, K) = (1, 1, 1, 136) for instance
    static constexpr bool kKernelComputesDelta =
        kIsHalf && (kOutputInRF || ArchTag::kMinComputeCapability != 70);

    static constexpr bool kNeedsAccumGradQ =
        !cutlass::platform::is_same<output_accum_t, output_t>::value;
    static constexpr bool kNeedsAccumGradK =
        !kOutputInRF && !cutlass::platform::is_same<output_accum_t, output_t>::value;
    static constexpr bool kNeedsAccumGradV =
        !kOutputInRF && !cutlass::platform::is_same<output_accum_t, output_t>::value;

    // Launch bounds
    static constexpr int64_t kNumThreads = kWarpSize * kNumWarpsPerBlock;
    static constexpr int64_t kMinBlocksPerSm =
        getWarpsPerSm<scalar_t, ArchTag>() / kNumWarpsPerBlock;

    using GemmType = DefaultGemmType<ArchTag, scalar_t>;
    using DefaultConfig =
        typename cutlass::gemm::device::DefaultGemmConfiguration<typename GemmType::OpClass,
                                                                 ArchTag,
                                                                 scalar_t,
                                                                 scalar_t,
                                                                 scalar_t,  // ElementC
                                                                 accum_t    // ElementAccumulator
                                                                 >;
    static constexpr auto kOptimalAlignement =
        cutlass::platform::max(DefaultConfig::kAlignmentA, DefaultConfig::kAlignmentB);
    static constexpr auto kMinimumAlignment = GemmType::kMinimumAlignment;

    struct MatmulQK {
        /*
        attn_T = k_j @ q_i.transpose(-2, -1) # matmul
        attn_T = (attn_T - logsumexp[i_start:i_end].unsqueeze(1).transpose(-2,
        -1)).exp() # epilogue

        with attn_T.shape = (kBlockSizeJ, kBlockSizeI)
        */
        using ThreadblockShape =
            cutlass::gemm::GemmShape<kBlockSizeJ, kBlockSizeI, GemmType::ThreadK>;
        using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
        using DefaultMma = typename cutlass::gemm::threadblock::DefaultMma<
            scalar_t,                   // ElementA
            cutlass::layout::RowMajor,  // LayoutA
            kIsAligned ? DefaultConfig::kAlignmentA : GemmType::kMinimumAlignment,
            scalar_t,                      // ElementB
            cutlass::layout::ColumnMajor,  // LayoutB
            kIsAligned ? DefaultConfig::kAlignmentB : GemmType::kMinimumAlignment,
            accum_t,                    // ElementC
            cutlass::layout::RowMajor,  // LayoutC
            typename GemmType::OpClass,
            ArchTag,
            ThreadblockShape,
            WarpShape,
            typename GemmType::InstructionShape,
            DefaultConfig::kStages,
            typename GemmType::Operator,
            false,  // AccumulatorsInRowMajor = false,
            cutlass::gemm::SharedMemoryClearOption::kNone>;
        using MmaCore = typename DefaultMma::MmaCore;
        using Mma = typename MakeCustomMma<typename DefaultMma::ThreadblockMma, kMaxK>::Mma;

        // used for efficient load of bias tile (Bij) from global memory to shared
        // memory
        using BiasLoader =
            TileSmemLoader<scalar_t,
                           // Bij is applied to transposed attn matrix tile (Pij.T). Bij is loaded
                           // row-major but needs to have transposed shape so we get the same
                           // elements.
                           cutlass::MatrixShape<ThreadblockShape::kN, ThreadblockShape::kM>,
                           MmaCore::kThreads,
                           // input restriction: kv_len has to be a multiple of this value
                           128 / cutlass::sizeof_bits<scalar_t>::value>;

        // Epilogue to store to shared-memory in a format that we can use later for
        // the second matmul
        using B2bGemm =
            typename cutlass::gemm::threadblock::B2bGemm<typename Mma::Operator::IteratorC,
                                                         typename Mma::Operator,
                                                         scalar_t,
                                                         WarpShape,
                                                         ThreadblockShape>;
        using AccumLambdaIterator =
            typename DefaultMmaAccumLambdaIterator<typename Mma::Operator::IteratorC,
                                                   accum_t,
                                                   kWarpSize>::Iterator;
        using AccumulatorSharedStorage = typename B2bGemm::AccumulatorSharedStorage;
    };

    struct MatmulGradV {
        /*
        grad_v[j_start:j_end] += attn_T @ do_i # matmul

        Dimensions: (kBlockSizeJ * kNumWarpsPerBlock, kBlockSizeI, K)
        (we might need to iterate multiple times on K)
        */
        using ThreadblockShape =
            cutlass::gemm::GemmShape<kBlockSizeJ, kBlockSizeI, GemmType::ThreadK>;
        using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
        using InstructionShape = typename GemmType::InstructionShape;

        using DefaultGemm =
            cutlass::gemm::kernel::DefaultGemm<scalar_t,                   // ElementA,
                                               cutlass::layout::RowMajor,  // LayoutA,
                                               DefaultConfig::kAlignmentA,
                                               scalar_t,                   // ElementB,
                                               cutlass::layout::RowMajor,  // LayoutB,
                                               kIsAligned ? DefaultConfig::kAlignmentB
                                                          : GemmType::kMinimumAlignment,
                                               output_t,
                                               cutlass::layout::RowMajor,  // LayoutC,
                                               accum_t,
                                               typename GemmType::OpClass,
                                               ArchTag,
                                               ThreadblockShape,
                                               WarpShape,
                                               typename GemmType::InstructionShape,
                                               typename DefaultConfig::EpilogueOutputOp,
                                               void,  // ThreadblockSwizzle - not used
                                               DefaultConfig::kStages,
                                               false,  // SplitKSerial
                                               typename GemmType::Operator>;

        // if dropout:
        //   for computing dVj += (Pij.T * Zij) @ dOi
        //   Pij_dropped.T = Pij.T * Zij is computed on the fly as fragments of
        //   Pij.T are loaded in. The reason we do it this way is because Pij.T and
        //   Zij are reused in later steps, while Pij_dropped.T is only needed in
        //   this step. computing Pij_dropped.T on the fly allows us to avoid
        //   keeping all 3 of Pij_dropped.T, Pij.T, and Zij in shared memory at the
        //   same time.
        // if no dropout:
        //   for computing dVj += Pij.T @ dOi
        using DefaultMmaFromSmem = typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
            typename DefaultGemm::Mma,
            typename MatmulQK::AccumulatorSharedStorage,
            kApplyDropout>;  // kScaleOperandA

        using Mma = typename DefaultMmaFromSmem::Mma;
        using WarpIteratorA = typename DefaultMmaFromSmem::WarpIteratorA;
        using IteratorB = typename Mma::IteratorB;
        using WarpCount = typename Mma::WarpCount;

        // Epilogue
        using DefaultOutputOp = typename DefaultConfig::EpilogueOutputOp;
        using DefaultEpilogue = typename DefaultGemm::Epilogue;
        using OutputTileIterator =
            typename cutlass::epilogue::threadblock::MakePrefetchableIterator<
                typename DefaultEpilogue::OutputTileIterator>::Iterator;
        using AccumTileGmem = GmemTile<typename Mma::FragmentC, (int)kNumThreads>;
    };

    struct MatmulDOIVJ {
        /*
        doi_t_vj = do_i @ v_j.transpose(-2, -1) # matmul
        tmp = (doi_t_vj - Di.unsqueeze(1)) * attn # inplace / epilogue?
        */
        using ThreadblockShape =
            cutlass::gemm::GemmShape<kBlockSizeI, kBlockSizeJ, GemmType::ThreadK>;
        using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;

        using ElementC = accum_t;  // CSY: Change it for better accuracy
        using ElementAccum = accum_t;

        // no-op output op - epilogue just stores result to global memory
        using BiasGradEpilogueOutputOp = typename cutlass::epilogue::thread::LinearCombination<
            ElementC,
            DefaultConfig::EpilogueOutputOp::kCount,
            typename DefaultConfig::EpilogueOutputOp::ElementAccumulator,
            typename DefaultConfig::EpilogueOutputOp::ElementCompute,
            cutlass::epilogue::thread::ScaleType::Nothing>;

        using DefaultGemm = typename cutlass::gemm::kernel::DefaultGemm<
            scalar_t,                   // ElementA
            cutlass::layout::RowMajor,  // LayoutA
            kIsAligned ? DefaultConfig::kAlignmentA : GemmType::kMinimumAlignment,
            scalar_t,                      // ElementB
            cutlass::layout::ColumnMajor,  // LayoutB
            kIsAligned ? DefaultConfig::kAlignmentB : GemmType::kMinimumAlignment,
            ElementC,                   // ElementC
            cutlass::layout::RowMajor,  // LayoutC
            ElementAccum,               // ElementAccumulator
            typename GemmType::OpClass,
            ArchTag,
            ThreadblockShape,
            WarpShape,
            typename GemmType::InstructionShape,
            BiasGradEpilogueOutputOp,  // EpilogueOutputOp
            void,                      // ThreadblockSwizzle (not used)
            // multiple preloads, dropout Zij tile, and 3 stages push us over shared
            // memory capacity on A100. set a ceiling on number of stages to save
            // shared memory if dropout is in use.
            kPreload && kApplyDropout && (kBlockSizeI * kBlockSizeJ > 64 * 64)
                ? cutlass::const_min(2, DefaultConfig::kStages)
                : DefaultConfig::kStages,  // Stages
            false,                         // SplitKSerial
            typename GemmType::Operator,
            cutlass::gemm::SharedMemoryClearOption::kNone>;
        using Mma = typename MakeCustomMma<typename DefaultGemm::Mma, kMaxK>::Mma;

        // epilogue used to write bias gradient, which is just the output of this
        // matmul with some operations applied to the fragment
        using BiasGradEpilogue = typename DefaultGemm::Epilogue;

        // Epilogue to store to shared-memory in a format that we can use later for
        // the second matmul
        using B2bGemm =
            typename cutlass::gemm::threadblock::B2bGemm<typename Mma::Operator::IteratorC,
                                                         typename Mma::Operator,
                                                         scalar_t,
                                                         WarpShape,
                                                         ThreadblockShape>;
        using AccumulatorSharedStorage = typename B2bGemm::AccumulatorSharedStorage;
    };

    struct MatmulGradQ {
        // grad_q <- tmp @ k_j
        using ThreadblockShape =
            cutlass::gemm::GemmShape<kBlockSizeI, kBlockSizeJ, GemmType::ThreadK>;
        using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
        using InstructionShape = typename GemmType::InstructionShape;

        using DefaultGemm =
            cutlass::gemm::kernel::DefaultGemm<scalar_t,                   // ElementA,
                                               cutlass::layout::RowMajor,  // LayoutA,
                                               DefaultConfig::kAlignmentA,
                                               scalar_t,                   // ElementB,
                                               cutlass::layout::RowMajor,  // LayoutB,
                                               kIsAligned ? DefaultConfig::kAlignmentB
                                                          : GemmType::kMinimumAlignment,
                                               output_t,
                                               cutlass::layout::RowMajor,  // LayoutC,
                                               accum_t,
                                               typename GemmType::OpClass,
                                               ArchTag,
                                               ThreadblockShape,
                                               WarpShape,
                                               typename GemmType::InstructionShape,
                                               typename DefaultConfig::EpilogueOutputOp,
                                               void,  // ThreadblockSwizzle - not used
                                               DefaultConfig::kStages,
                                               false,  // SplitKSerial
                                               typename GemmType::Operator>;

        using DefaultMmaFromSmem = typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
            typename DefaultGemm::Mma,
            typename MatmulDOIVJ::AccumulatorSharedStorage,
            false>;  // kScaleOperandA
        using Mma = typename DefaultMmaFromSmem::Mma;
        using IteratorB = typename Mma::IteratorB;
        using WarpCount = typename Mma::WarpCount;

        // Epilogue
        using DefaultOutputOp = typename DefaultConfig::EpilogueOutputOp;
        using DefaultEpilogue = typename DefaultGemm::Epilogue;
        using OutputTileIterator =
            typename cutlass::epilogue::threadblock::MakePrefetchableIterator<
                typename DefaultEpilogue::OutputTileIterator>::Iterator;
        using AccumTileGmem = GmemTile<typename Mma::FragmentC, (int)kNumThreads>;
    };
    struct MatmulGradK {
        // grad_k <- tmp.transpose(-2, -1) @ q_i
        using ThreadblockShape =
            cutlass::gemm::GemmShape<kBlockSizeJ, kBlockSizeI, GemmType::ThreadK>;
        using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
        using InstructionShape = typename GemmType::InstructionShape;

        using DefaultGemm =
            cutlass::gemm::kernel::DefaultGemm<scalar_t,                   // ElementA,
                                               cutlass::layout::RowMajor,  // LayoutA,
                                               DefaultConfig::kAlignmentA,
                                               scalar_t,                   // ElementB,
                                               cutlass::layout::RowMajor,  // LayoutB,
                                               kIsAligned ? DefaultConfig::kAlignmentB
                                                          : GemmType::kMinimumAlignment,
                                               output_t,
                                               cutlass::layout::RowMajor,  // LayoutC,
                                               accum_t,
                                               typename GemmType::OpClass,
                                               ArchTag,
                                               ThreadblockShape,
                                               WarpShape,
                                               typename GemmType::InstructionShape,
                                               typename DefaultConfig::EpilogueOutputOp,
                                               void,  // ThreadblockSwizzle - not used
                                               DefaultConfig::kStages,
                                               false,  // SplitKSerial
                                               typename GemmType::Operator>;

        using DefaultMmaFromSmemN = typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
            typename DefaultGemm::Mma,
            typename MatmulQK::AccumulatorSharedStorage,
            false>;  // kScaleOperandA
        using DefaultMmaFromSmemT = typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
            typename DefaultGemm::Mma,
            typename MatmulDOIVJ::AccumulatorSharedStorage,
            false,      // kScaleOperandA
            kPreload>;  // kTransposeA
        using DefaultMmaFromSmem =
            typename cutlass::platform::conditional<DefaultMmaFromSmemT::kIsTransposedA,
                                                    DefaultMmaFromSmemT,
                                                    DefaultMmaFromSmemN>::type;
        using Mma = typename DefaultMmaFromSmem::Mma;
        using IteratorB = typename Mma::IteratorB;
        using WarpCount = typename Mma::WarpCount;

        // Epilogue
        using DefaultOutputOp = typename DefaultConfig::EpilogueOutputOp;
        using DefaultEpilogue = typename DefaultGemm::Epilogue;
        using OutputTileIterator =
            typename cutlass::epilogue::threadblock::MakePrefetchableIterator<
                typename DefaultEpilogue::OutputTileIterator>::Iterator;
        using AccumTileGmem = GmemTile<typename Mma::FragmentC, (int)kNumThreads>;
    };

    using broadcast_1 = Broadcast1_<typename MatmulQK::BiasLoader::ThreadMap,
                                    typename MatmulQK::BiasLoader::Shape,
                                    scalar_t>;
    using broadcast_2 = Broadcast2_<typename MatmulQK::BiasLoader::ThreadMap,
                                    typename MatmulQK::BiasLoader::Shape,
                                    scalar_t>;

    // shared storage for keeping Zij matrix. not needed if we aren't using
    // dropout, in which case we use an empty array to save shared memory
    using ZijSharedStorage = typename cutlass::platform::conditional<
        kApplyDropout,
        typename MatmulQK::AccumulatorSharedStorage,
        // dummy shared storage object that takes up no space.
        typename cutlass::gemm::threadblock::AccumulatorSharedStorage<
#ifdef _WIN32
            // windows builds throw the error:
            // "type containing an unknown-size array is not allowed"
            // if we try to make Zij shared storage zero-sized.
            // To get around this just make it sized 1 on windows.
            typename cutlass::gemm::GemmShape<1, 1, 0>,
#else
            typename cutlass::gemm::GemmShape<0, 0, 0>,
#endif
            typename MatmulQK::AccumulatorSharedStorage::Element,
            typename MatmulQK::AccumulatorSharedStorage::Layout,
            typename cutlass::MatrixShape<0, 0>>>::type;

    struct SharedStoragePrologue {
        struct {
            cutlass::Array<accum_t, kBlockSizeI> di;  // (do_i * o_i).sum(-1)
            typename MatmulQK::Mma::SharedStorageA mm_qk_k;
        } persistent;
        union {
            struct {
                // part1 - after Q.K / dV / dO.V
                union {
                    // 1. efficient load of bias tile Bij, which is then applied to Pij
                    // typename MatmulQK::BiasLoader::SmemTile bias;
                    cutlass::AlignedBuffer<float, MatmulQK::BiasLoader::Shape::kCount> bias;
                    // 4. store Pij. it is needed:
                    // - in dVj += (Pij.T * Zij) @ dOi
                    // - in dSij = Pij * (dPij - Di)
                    // 6. dVj += (Pij.T * Zij) @ dOi
                    // 10. write to fragment
                    typename MatmulQK::AccumulatorSharedStorage attn_shared_storage;
                };
                // 5. store Zij. it is needed:
                // - to compute Pij_dropped = Pij * Zij on the fly as fragments of Pij
                // are loaded for the computation of dVj.
                // - to compute dPij = (dOi @ Vj.T) * Zij
                // 6. used in dVj += (Pij.T * Zij) @ dOi
                // 9. used in dPij = dPij_dropped * Zij
                ZijSharedStorage zij;

                union {
                    // 2. prologue for dVj
                    // 6. workspace for dVj += (Pij.T * Zij) @ dOi
                    typename MatmulGradV::Mma::SharedStorage mm_gradV;
                    // 7. dVj epilogue
                    typename MatmulGradV::DefaultEpilogue::SharedStorage gradV_epilogue;
                };

                // 3. prologue for dPij_dropped
                // 8. used in dPij_dropped = dOi @ Vj.T
                typename MatmulDOIVJ::Mma::SharedStorage mm_doivj;
            } part1;

            struct {
                // part2 - dQ
                union {
                    typename MatmulQK::AccumulatorSharedStorage
                        tmpT_shared_storage;  // (from part1)
                    typename MatmulDOIVJ::AccumulatorSharedStorage tmp_shared_storage;
                };
                typename MatmulGradK::Mma::SharedStorage mm_gradK;  // (preload)
                typename MatmulGradQ::Mma::SharedStorage mm_gradQ;  // (preload)
                union {
                    // store dB = dSij to global memory
                    typename MatmulDOIVJ::BiasGradEpilogue::SharedStorage gradB_epilogue;
                    typename MatmulGradQ::DefaultEpilogue::SharedStorage gradQ_epilogue;
                };

            } part2;

            struct {
                // part3 - after last iteration on dQ's epilogue / dK
                union {
                    typename MatmulQK::AccumulatorSharedStorage
                        tmpT_shared_storage;  // (from part1)
                    typename MatmulDOIVJ::AccumulatorSharedStorage tmp_shared_storage;
                };
                typename MatmulGradK::Mma::SharedStorage mm_gradK;  // (preload)
                typename MatmulGradQ::DefaultEpilogue::SharedStorage gradQ_epilogue_lastIter;

                typename MatmulGradK::DefaultEpilogue::SharedStorage gradK_epilogue;
            } part3;

            struct {
                // part4 - after last iteration on dK's epilogue / preload next K.Q_t
                typename MatmulQK::Mma::SharedStorageB mm_qk_q;

                // If we reach end of current key, dump RF->gmem with "final" epilogues
                typename MatmulGradK::DefaultEpilogue::SharedStorage gradK_epilogue_final;
                typename MatmulGradV::DefaultEpilogue::SharedStorage gradV_epilogue_final;
            } part4;
        };
// ===========================================
#define FIELD(INSIDE_STRUCT, FIELDNAME) \
    CUTLASS_DEVICE auto& FIELDNAME()    \
    {                                   \
        return INSIDE_STRUCT.FIELDNAME; \
    }

        FIELD(persistent, di)
        FIELD(persistent, mm_qk_k)
        FIELD(part1, bias)
        FIELD(part1, attn_shared_storage)
        FIELD(part1, zij)
        FIELD(part1, mm_gradV)
        FIELD(part1, gradV_epilogue)
        FIELD(part1, mm_doivj)
        FIELD(part2, mm_gradK)
        FIELD(part2, mm_gradQ)
        FIELD(part2, gradB_epilogue)
        FIELD(part2, gradQ_epilogue)
        FIELD(part2, tmp_shared_storage)
        FIELD(part3, tmpT_shared_storage)
        FIELD(part3, gradQ_epilogue_lastIter)
        FIELD(part3, gradK_epilogue)
        FIELD(part4, mm_qk_q)
        FIELD(part4, gradK_epilogue_final)
        FIELD(part4, gradV_epilogue_final)
    };

    struct SharedStorageNoPrologue {
        struct {
            cutlass::Array<accum_t, kBlockSizeI> di;  // (do_i * o_i).sum(-1)
        } persistent;
        union {
            struct {
                // part1 - Q.K matmul
                typename MatmulQK::Mma::SharedStorageA mm_qk_k;
                typename MatmulQK::Mma::SharedStorageB mm_qk_q;
            } part1;

            struct {
                // part2 - compute gradV
                union {
                    // 1. efficient load of bias tile Bij, which is then applied to Pij
                    cutlass::AlignedBuffer<float, MatmulQK::BiasLoader::Shape::kCount> bias;
                    // 2. store Pij to shared memory. it is needed:
                    // - in this step, where it is used in dVj += (Pij.T * Zij) @ dOi
                    // - in next step where it is used in dSij = Pij * (dPij - Di)
                    typename MatmulQK::AccumulatorSharedStorage attn_shared_storage;
                };
                // 3. store Zij. it is needed:
                // - in this step, where it is used to compute Pij_dropped = Pij * Zij
                // on the
                //   fly as fragments of Pij are loaded for the computation of dVj.
                // - later to compute dPij = (dOi @ Vj.T) * Zij
                ZijSharedStorage zij;

                union {
                    typename MatmulGradV::Mma::SharedStorage mm_gradV;
                    typename MatmulGradV::DefaultEpilogue::SharedStorage gradV_epilogue;
                };
            } part2;

            struct {
                // part3 - DO.V matmul
                union {
                    // first compute dPij = (dOi @ Vj.T) * Zij
                    // and dSij = Pij * (dPij - Di)
                    struct {
                        // (from part2) - Pij for computing dSij = Pij * (dPij - Di)
                        typename MatmulQK::AccumulatorSharedStorage attn_shared_storage;
                        // (from part2) - Zij for computing dPij = dPij_dropped * Zij
                        ZijSharedStorage zij;
                        // matmul to compute dOiVj
                        typename MatmulDOIVJ::Mma::SharedStorage mm_doivj;
                    };
                    // then store dB = dSij to global memory
                    typename MatmulDOIVJ::BiasGradEpilogue::SharedStorage gradB_epilogue;
                };
            } part3;

            struct {
                // part4 - compute gradQ
                typename MatmulQK::AccumulatorSharedStorage tmpT_shared_storage;  // (from part2)
                typename MatmulDOIVJ::AccumulatorSharedStorage tmp_shared_storage;
                union {
                    typename MatmulGradQ::Mma::SharedStorage mm_gradQ;
                    typename MatmulGradQ::DefaultEpilogue::SharedStorage gradQ_epilogue;
                    typename MatmulGradQ::DefaultEpilogue::SharedStorage gradQ_epilogue_lastIter;
                };
            } part4;

            struct {
                // part5 - compute gradK
                typename MatmulQK::AccumulatorSharedStorage tmpT_shared_storage;  // (from part2)
                typename MatmulDOIVJ::AccumulatorSharedStorage tmp_shared_storage;
                union {
                    typename MatmulGradK::Mma::SharedStorage mm_gradK;
                    typename MatmulGradK::DefaultEpilogue::SharedStorage gradK_epilogue;
                };
            } part5;

            struct {
                // part6 - store RF accumulated into gmem
                typename MatmulGradK::DefaultEpilogue::SharedStorage gradK_epilogue_final;
                typename MatmulGradV::DefaultEpilogue::SharedStorage gradV_epilogue_final;
            } part6;
        };
// ===========================================
#define FIELD(INSIDE_STRUCT, FIELDNAME) \
    CUTLASS_DEVICE auto& FIELDNAME()    \
    {                                   \
        return INSIDE_STRUCT.FIELDNAME; \
    }

        FIELD(persistent, di)
        FIELD(part1, mm_qk_k)
        FIELD(part1, mm_qk_q)
        FIELD(part2, bias)
        FIELD(part2, attn_shared_storage)
        FIELD(part2, zij)
        FIELD(part2, mm_gradV)
        FIELD(part2, gradV_epilogue)
        FIELD(part3, mm_doivj)
        FIELD(part3, gradB_epilogue)
        FIELD(part4, tmpT_shared_storage)
        FIELD(part4, tmp_shared_storage)
        FIELD(part4, mm_gradQ)
        FIELD(part4, gradQ_epilogue)
        FIELD(part4, gradQ_epilogue_lastIter)
        FIELD(part5, mm_gradK)
        FIELD(part5, gradK_epilogue)
        FIELD(part6, gradK_epilogue_final)
        FIELD(part6, gradV_epilogue_final)
    };

    using SharedStorage = typename cutlass::platform::
        conditional<kPreload, SharedStoragePrologue, SharedStorageNoPrologue>::type;

    struct OutputFragments {
        typename MatmulGradV::Mma::FragmentC gradV;
        typename MatmulGradK::Mma::FragmentC gradK;

        CUTLASS_DEVICE void clear()
        {
            gradV.clear();
            gradK.clear();
        }
    };

    static bool __host__ check_supported(Params const& p)
    {
        CHECK_ALIGNED_PTR(p.query_ptr, kMinimumAlignment);
        CHECK_ALIGNED_PTR(p.key_ptr, kMinimumAlignment);
        CHECK_ALIGNED_PTR(p.value_ptr, kMinimumAlignment);
        CHECK_ALIGNED_PTR(p.output_ptr, kMinimumAlignment);
        CHECK_ALIGNED_PTR(p.grad_output_ptr, kMinimumAlignment);
        EVOFORMER_CHECK(p.lse_strideH % 8 == 0, "LSE is not correctly aligned");
        EVOFORMER_CHECK(p.lse_strideB % 8 == 0, "LSE is not correctly aligned");
        EVOFORMER_CHECK(p.num_heads <= 1 || p.q_strideH % kMinimumAlignment == 0,
                        "query is not correctly aligned (strideH)");
        EVOFORMER_CHECK(p.num_heads <= 1 || p.k_strideH % kMinimumAlignment == 0,
                        "key is not correctly aligned (strideH)");
        EVOFORMER_CHECK(p.num_heads <= 1 || p.v_strideH % kMinimumAlignment == 0,
                        "value is not correctly aligned (strideH)");
        EVOFORMER_CHECK(p.num_batches <= 1 || p.q_strideB % kMinimumAlignment == 0,
                        "query is not correctly aligned (strideB)");
        EVOFORMER_CHECK(p.num_batches <= 1 || p.k_strideB % kMinimumAlignment == 0,
                        "key is not correctly aligned (strideB)");
        EVOFORMER_CHECK(p.num_batches <= 1 || p.v_strideB % kMinimumAlignment == 0,
                        "value is not correctly aligned (strideB)");
        EVOFORMER_CHECK(p.q_strideM % kMinimumAlignment == 0,
                        "query is not correctly aligned (strideM)");
        EVOFORMER_CHECK(p.k_strideM % kMinimumAlignment == 0,
                        "key is not correctly aligned (strideM)");
        EVOFORMER_CHECK(p.v_strideM % kMinimumAlignment == 0,
                        "value is not correctly aligned (strideM)");
        EVOFORMER_CHECK(p.dropout_prob <= 1.0f && p.dropout_prob >= 0.0f,
                        "Invalid value for `dropout_prob`");
        EVOFORMER_CHECK(kApplyDropout || p.dropout_prob == 0.0f,
                        "Set `kApplyDropout`=True to support `dropout_prob > 0`");
        EVOFORMER_CHECK(p.head_dim > 0, "Invalid value for `head_dim`");
        EVOFORMER_CHECK(p.head_dim_value > 0, "Invalid value for `head_dim_value`");
        EVOFORMER_CHECK(p.num_queries > 0, "Invalid value for `num_queries`");
        EVOFORMER_CHECK(p.num_keys > 0, "Invalid value for `num_keys`");
        EVOFORMER_CHECK(p.num_heads > 0, "Invalid value for `num_heads`");
        EVOFORMER_CHECK(p.num_batches > 0, "Invalid value for `num_batches`");
        EVOFORMER_CHECK(p.head_dim <= kMaxK, "kMaxK: Expected `head_dim < kMaxK`");
        EVOFORMER_CHECK(p.head_dim_value <= kMaxK, "kMaxK: Expected `head_dim_value < kMaxK`");
        return true;
    }

    static CUTLASS_DEVICE void attention_kernel(Params p)
    {
        extern __shared__ char smem_buffer[];
        SharedStorage& shared_storage = *((SharedStorage*)smem_buffer);

        uint16_t thread_id = threadIdx.x;
        uint8_t warp_id = warp_uniform(thread_id / 32);
        uint8_t lane_id = thread_id % 32;

        if (kPrologueQK) {
            prologueQkNextIteration<true>(shared_storage, p, 0, 0, warp_id, lane_id);
        }

        // Computes (dO*out).sum(-1) and writes it to `p.delta_ptr`
        if (kKernelComputesDelta) {
            constexpr int kOptimalElements = 128 / cutlass::sizeof_bits<scalar_t>::value;
            if (p.head_dim_value % kOptimalElements == 0) {
                for (int query_start = 0; query_start < p.num_queries; query_start += kBlockSizeI) {
                    computeDelta<kOptimalElements>(p, query_start, warp_id, lane_id);
                }
            } else {
                for (int query_start = 0; query_start < p.num_queries; query_start += kBlockSizeI) {
                    computeDelta<1>(p, query_start, warp_id, lane_id);
                }
            }
            __syncthreads();
        }

        OutputFragments output_frags;

        int32_t key_start = 0;
        int32_t key_end = p.num_keys / kBlockSizeJ * kBlockSizeJ;
        for (; key_start < key_end; key_start += kBlockSizeJ) {
            output_frags.clear();
            int32_t query_start = getQueryStart(p, key_start);
            int32_t query_end =
                query_start + (p.num_queries - query_start) / kBlockSizeI * kBlockSizeI;
            for (; query_start < query_end; query_start += kBlockSizeI) {
                processBlockIJ<true>(
                    shared_storage, output_frags, p, query_start, key_start, warp_id, lane_id);
            }
            // last (partial) query
            if (query_start < p.num_queries) {
                processBlockIJ<false>(
                    shared_storage, output_frags, p, query_start, key_start, warp_id, lane_id);
            }
            if (kOutputInRF) {
                writeFragsToGmem<true>(
                    shared_storage, output_frags, p, key_start, warp_id, lane_id);
            } else if (getQueryStart(p, key_start) >= p.num_queries) {
                zfillGradKV<true>(p, key_start, warp_id, lane_id);
            }
            __syncthreads();
        }
        // Last (partial) key
        if (key_start != p.num_keys) {
            output_frags.clear();
            int32_t query_start = getQueryStart(p, key_start);
            for (; query_start < p.num_queries; query_start += kBlockSizeI) {
                warp_id = warp_uniform(warp_id);
                processBlockIJ<false>(
                    shared_storage, output_frags, p, query_start, key_start, warp_id, lane_id);
            }
            if (kOutputInRF) {
                writeFragsToGmem<false>(
                    shared_storage, output_frags, p, key_start, warp_id, lane_id);
            } else if (getQueryStart(p, key_start) >= p.num_queries) {
                zfillGradKV<false>(p, key_start, warp_id, lane_id);
            }
        }
    }

    static CUTLASS_DEVICE void loadDi(cutlass::Array<accum_t, kBlockSizeI>& di,
                                      Params const& p,
                                      int32_t query_start)
    {
        int32_t thread_id = threadIdx.x + threadIdx.y * blockDim.x;
        if (thread_id < kBlockSizeI) {
            accum_t di_rf = accum_t(0);
            if (query_start + thread_id < p.num_queries) {
                di_rf = p.delta_ptr[query_start + thread_id];
            }
            di[thread_id] = di_rf;
        }
    }

    template <bool skipBoundsChecks>
    static CUTLASS_DEVICE void zfillGradKV(Params const& p,
                                           int32_t key_start,
                                           uint8_t warp_id,
                                           uint8_t lane_id)
    {
        constexpr int kThreadsPerKey = 8;
        constexpr int kParallelKeys = kNumThreads / kThreadsPerKey;
        static_assert(kBlockSizeJ % kParallelKeys == 0, "");
        // This function is not really optimized, but should rarely be used
        // It's only used when some keys are "useless" and don't attend to
        // any query, due to causal masking
        int thread_id = 32 * warp_id + lane_id;
        int k_shift = lane_id % kThreadsPerKey;

        CUTLASS_PRAGMA_UNROLL
        for (int j = 0; j < kBlockSizeJ; j += kParallelKeys) {
            int key = key_start + j + (thread_id / kThreadsPerKey);
            if (!skipBoundsChecks && key >= p.num_keys) { continue; }
            auto gv_ptr = p.grad_value_ptr + key * p.gV_strideM();
            auto gk_ptr = p.grad_key_ptr + key * p.gK_strideM();

            for (int k = k_shift; k < p.head_dim_value; k += kThreadsPerKey) {
                gv_ptr[k] = scalar_t(0);
            }
            for (int k = k_shift; k < p.head_dim; k += kThreadsPerKey) { gk_ptr[k] = scalar_t(0); }
        }
    }

    template <bool skipBoundsChecks>
    static CUTLASS_DEVICE void processBlockIJ(SharedStorage& shared_storage,
                                              OutputFragments& output_frags,
                                              Params& p,
                                              int32_t query_start,
                                              int32_t key_start,
                                              uint8_t warp_id,
                                              uint8_t lane_id)
    {
        cutlass::MatrixCoord no_offset{0, 0};
        accum_t scale = p.scale;
        int16_t thread_id = 32 * warp_id + lane_id;
        auto rematerializeThreadIds = [&]() {
            // Prevents `nvcc` from keeping values deduced from
            // `thread_id`, `warp_id`, ... in RF - to reduce register pressure
            warp_id = warp_uniform(thread_id / 32);
            lane_id = thread_id % 32;
            thread_id = 32 * warp_id + lane_id;
        };

        bool isFirstQuery = (query_start == getQueryStart(p, key_start));
        int32_t next_query, next_key;
        incrIteration(p, query_start, key_start, next_query, next_key);
        bool isLastQuery = next_key != key_start;
        __syncthreads();
        loadDi(shared_storage.di(), p, query_start);

        int32_t num_queries_in_block =
            skipBoundsChecks ? MatmulQK::Mma::Shape::kN
                             : warp_uniform(cutlass::fast_min((int32_t)MatmulQK::Mma::Shape::kN,
                                                              p.num_queries - query_start));
        int32_t num_keys_in_block =
            skipBoundsChecks ? MatmulQK::Mma::Shape::kM
                             : warp_uniform(cutlass::fast_min((int32_t)MatmulQK::Mma::Shape::kM,
                                                              p.num_keys - key_start));

        auto prologueGradV = [&](int col) {
            typename MatmulGradV::Mma::IteratorB iterator_dO(
                {int32_t(p.gO_strideM)},
                p.grad_output_ptr + query_start * p.gO_strideM + col,
                {num_queries_in_block, p.head_dim_value - col},
                thread_id,
                no_offset);
            MatmulGradV::Mma::prologue(
                shared_storage.mm_gradV(), iterator_dO, thread_id, num_queries_in_block);
        };
        auto prologueGradQ = [&](int col) {
            typename MatmulGradQ::Mma::IteratorB iterator_K(
                {int32_t(p.k_strideM)},
                p.key_ptr + key_start * p.k_strideM + col,
                {num_keys_in_block, p.head_dim - col},
                thread_id,
                no_offset);
            MatmulGradQ::Mma::prologue(
                shared_storage.mm_gradQ(), iterator_K, thread_id, num_keys_in_block);
        };
        auto prologueGradK = [&](int col) {
            typename MatmulGradK::Mma::IteratorB iterator_Q(
                {int32_t(p.q_strideM)},
                p.query_ptr + query_start * p.q_strideM + col,
                {num_queries_in_block, p.head_dim - col},
                thread_id,
                no_offset);
            MatmulGradK::Mma::prologue(
                shared_storage.mm_gradK(), iterator_Q, thread_id, num_queries_in_block);
        };
        auto prologueDOV = [&]() {
            typename MatmulDOIVJ::Mma::IteratorA iterator_A(
                {int32_t(p.gO_strideM)},
                p.grad_output_ptr + query_start * p.gO_strideM,
                {num_queries_in_block, p.head_dim_value},
                thread_id,
                no_offset);
            typename MatmulDOIVJ::Mma::IteratorB iterator_B({int32_t(p.v_strideM)},
                                                            p.value_ptr + key_start * p.v_strideM,
                                                            {p.head_dim_value, num_keys_in_block},
                                                            thread_id,
                                                            no_offset);
            MatmulDOIVJ::Mma::prologue(
                shared_storage.mm_doivj(), iterator_A, iterator_B, thread_id, p.head_dim_value);
        };

        /////////////////////////////////////////////////////////////////////////////////////////////////
        // MatmulQK
        /////////////////////////////////////////////////////////////////////////////////////////////////
        {
            using Mma = typename MatmulQK::Mma;

            cutlass::gemm::GemmCoord problem_size(num_keys_in_block,
                                                  num_queries_in_block,
                                                  p.head_dim  // k
            );

            // k_j
            typename Mma::IteratorA iterator_A({int32_t(p.k_strideM)},
                                               p.key_ptr + key_start * p.k_strideM,
                                               {problem_size.m(), problem_size.k()},
                                               thread_id,
                                               no_offset);

            // q_i.transpose(-2, -1)
            typename Mma::IteratorB iterator_B({int32_t(p.q_strideM)},
                                               p.query_ptr + query_start * p.q_strideM,
                                               {problem_size.k(), problem_size.n()},
                                               thread_id,
                                               no_offset);

            Mma mma(
                shared_storage.mm_qk_k(), shared_storage.mm_qk_q(), thread_id, warp_id, lane_id);

            typename Mma::FragmentC accum;

            accum.clear();

            auto gemm_k_iterations = (problem_size.k() + Mma::Shape::kK - 1) / Mma::Shape::kK;

            // Compute threadblock-scoped matrix multiply-add
            mma.set_prologue_done(kPrologueQK);
            mma.set_zero_outside_bounds(!skipBoundsChecks);
            mma(gemm_k_iterations, accum, iterator_A, iterator_B, accum);

            // Epilogue: add LSE + exp and store that to our shared memory buffer
            // shmem <- (matmul_result -
            // logsumexp[i_start:i_end].unsqueeze(1)).exp()
            int warp_idx_mn_0 = warp_id % (Mma::Base::WarpCount::kM * Mma::Base::WarpCount::kN);
            auto output_tile_coords = cutlass::MatrixCoord{
                warp_idx_mn_0 % Mma::Base::WarpCount::kM, warp_idx_mn_0 / Mma::Base::WarpCount::kM};

            if (broadcast_1::kEnable || broadcast_2::kEnable) {
                cutlass::TensorRef<float, cutlass::layout::RowMajor> bias_tensor_ref(
                    shared_storage.bias().data(),
                    cutlass::layout::RowMajor(MatmulQK::ThreadblockShape::kM));
                using Shape = cutlass::MatrixShape<MatmulQK::ThreadblockShape::kM,
                                                   MatmulQK::ThreadblockShape::kN>;
                AttentionBiasEpilogue<Shape,
                                      scalar_t,
                                      MatmulQK::MmaCore::kThreads,
                                      Broadcast1_,
                                      Broadcast2_>
                    bias_epilogue;
                bias_epilogue(bias_tensor_ref,
                              p.bias1_ptr + key_start,
                              p.bias2_ptr + query_start * p.num_keys + key_start,
                              thread_id,
                              {num_queries_in_block, num_keys_in_block},
                              p.num_keys);
                // Pij += Bij, Pij is in register fragment and Bij is in shared memory
                auto lane_offset = MatmulQK::AccumLambdaIterator::get_lane_offset(
                    lane_id, warp_id, output_tile_coords);
                MatmulQK::AccumLambdaIterator::iterateRows(
                    lane_offset,
                    [&](int accum_n) {},
                    [&](int accum_m, int accum_n, int idx) {
                        // remember we are transposed
                        accum[idx] = accum[idx] * scale + bias_tensor_ref.at({accum_n, accum_m});
                    },
                    [&](int accum_n) {});
            } else {
                accum = cutlass::multiplies<typename Mma::FragmentC>()(scale, accum);
            }

            __syncthreads();
            if (kPrologueGV) { prologueGradV(0); }
            if (kPrologueDOV) { prologueDOV(); }

            MatmulQK::B2bGemm::accumApplyLSEToSmem(shared_storage.attn_shared_storage(),
                                                   accum,
                                                   p.logsumexp_ptr + query_start,
                                                   problem_size.n(),
                                                   thread_id,
                                                   warp_id,
                                                   lane_id,
                                                   output_tile_coords);

            __syncthreads();
        }
        rematerializeThreadIds();

        /////////////////////////////////////////////////////////////////////////////////////////////////
        // GradV matmul
        //
        // grad_v[j_start:j_end] += attn_T @ do_i
        /////////////////////////////////////////////////////////////////////////////////////////////////
        constexpr bool kSingleIterationGradV = kMaxK <= MatmulGradV::ThreadblockShape::kN;
        for (int col = 0; col < (kSingleIterationGradV ? 1 : p.head_dim_value);
             col += MatmulGradV::ThreadblockShape::kN) {
            using Mma = typename MatmulGradV::Mma;
            using AccumTileGmem = typename MatmulGradQ::AccumTileGmem;

            cutlass::gemm::GemmCoord problem_size(
                num_keys_in_block, p.head_dim_value - col, num_queries_in_block);
            auto createEpilogueIter = [&]() {
                return typename MatmulGradV::OutputTileIterator(
                    typename MatmulGradV::OutputTileIterator::Params{p.gV_strideM()},
                    p.grad_value_ptr + key_start * p.gV_strideM() + col,
                    {num_keys_in_block, p.head_dim_value - col},
                    thread_id);
            };
            typename Mma::IteratorB iterator_B({int32_t(p.gO_strideM)},
                                               p.grad_output_ptr + query_start * p.gO_strideM + col,
                                               {num_queries_in_block, p.head_dim_value - col},
                                               thread_id,
                                               no_offset);

            // if dropout: dVj += (Pij.T * Zij) @ dOi
            // otherwise:  dVj += Pij.T @ dOi
            Mma mma(shared_storage.mm_gradV(),
                    // operand A: Pij
                    typename MatmulGradV::WarpIteratorA(
                        shared_storage.attn_shared_storage().accum_ref(), lane_id),
                    // if we're using dropout, operand A is Pij_dropped = Pij * Zij
                    // which is computed on the fly as fragments of Pij are loaded in
                    typename Mma::WarpIteratorAScale(shared_storage.zij().accum_ref(), lane_id),
                    thread_id,
                    warp_id,
                    lane_id);

            int storage_id = col / MatmulGradV::ThreadblockShape::kN;
            AccumTileGmem gmem_tile{p.workspace_gv + storage_id * AccumTileGmem::kElementsStored};
            if (!kOutputInRF) {
                if (isFirstQuery || !kNeedsAccumGradV) {
                    output_frags.gradV.clear();
                } else {
                    gmem_tile.load(output_frags.gradV, thread_id);
                }
            }
            mma.set_prologue_done(kPrologueGV);

            auto gemm_k_iterations = (problem_size.k() + Mma::Shape::kK - 1) / Mma::Shape::kK;

            // Compute threadblock-scoped matrix multiply-add
            __syncthreads();

            mma(gemm_k_iterations, output_frags.gradV, iterator_B, output_frags.gradV);
            __syncthreads();
            if (kPrologueGV && !kSingleIterationGradV &&
                col + MatmulGradV::ThreadblockShape::kN < p.head_dim_value) {
                prologueGradV(col + MatmulGradV::ThreadblockShape::kN);
            }

            if (!kOutputInRF) {
                if (kNeedsAccumGradV && !isLastQuery) {
                    gmem_tile.store(output_frags.gradV, thread_id);
                } else {
                    accumulateInGmem<MatmulGradV>(shared_storage.gradV_epilogue(),
                                                  output_frags.gradV,
                                                  createEpilogueIter(),
                                                  isFirstQuery || kNeedsAccumGradV,
                                                  warp_id,
                                                  lane_id);
                }
            }
        }
        __syncthreads();
        /////////////////////////////////////////////////////////////////////////////////////////////////
        // MatmulDOIVJ
        /////////////////////////////////////////////////////////////////////////////////////////////////
        {
            using Mma = typename MatmulDOIVJ::Mma;
            // do_i
            typename Mma::IteratorA iterator_A({int32_t(p.gO_strideM)},
                                               p.grad_output_ptr + query_start * p.gO_strideM,
                                               {num_queries_in_block, p.head_dim_value},
                                               thread_id,
                                               no_offset);

            // v_j.transpose(-2, -1)
            typename Mma::IteratorB iterator_B({int32_t(p.v_strideM)},
                                               p.value_ptr + key_start * p.v_strideM,
                                               {p.head_dim_value, num_keys_in_block},
                                               thread_id,
                                               no_offset);

            Mma mma(shared_storage.mm_doivj(), thread_id, warp_id, lane_id);
            mma.set_prologue_done(kPrologueDOV);
            mma.set_zero_outside_bounds(!skipBoundsChecks);

            typename Mma::FragmentC accum;

            accum.clear();

            auto gemm_k_iterations = (p.head_dim_value + Mma::Shape::kK - 1) / Mma::Shape::kK;

            // Compute threadblock-scoped matrix multiply-add
            mma(gemm_k_iterations, accum, iterator_A, iterator_B, accum);
            __syncthreads();
            if (kPrologueGQ) { prologueGradQ(0); }
            if (kPrologueGK) { prologueGradK(0); }

            int warp_idx_mn_0 = warp_id % (Mma::Base::WarpCount::kM * Mma::Base::WarpCount::kN);
            auto output_tile_coords = cutlass::MatrixCoord{
                warp_idx_mn_0 % Mma::Base::WarpCount::kM, warp_idx_mn_0 / Mma::Base::WarpCount::kM};
            // TODO: This must be terribly inefficient. There must be a better way
            // tmp [RF] <- (accum [RF] - Di [smem] ) * attn_T.T [smem]
            // attn_shared_storage  [smem] <- tmp.T
            // tmp_shared_storage [smem] <- tmp
            {
                using LambdaIterator =
                    typename DefaultMmaAccumLambdaIterator<typename Mma::Operator::IteratorC,
                                                           typename MatmulDOIVJ::ElementAccum,
                                                           kWarpSize>::Iterator;
                auto lane_offset =
                    LambdaIterator::get_lane_offset(lane_id, warp_id, output_tile_coords);

                auto attn_T = shared_storage.attn_shared_storage().accum_ref();
                accum_t current_di;
                // dSij = (dPij - Di) * Pij
                LambdaIterator::iterateRows(
                    lane_offset,
                    [&](int accum_m) { current_di = shared_storage.di()[accum_m]; },
                    [&](int accum_m, int accum_n, int idx) {
                        if (skipBoundsChecks ||
                            (accum_m < num_queries_in_block && accum_n < num_keys_in_block)) {
                            accum_t attn = attn_T.at({accum_n, accum_m});
                            accum[idx] = (accum[idx] - current_di) * attn;
                        } else {
                            accum[idx] = 0;
                        }
                    },
                    [&](int accum_m) {

                    });

                using DefaultGemm = typename MatmulDOIVJ::DefaultGemm;
                using OutputOp = typename MatmulDOIVJ::BiasGradEpilogueOutputOp;
                if (broadcast_1::kEnable && p.grad_bias1_ptr) {
                    using Epilogue =
                        typename BiasGradEpilogueAffineRankN<ArchTag,
                                                             2,
                                                             typename MatmulDOIVJ::ThreadblockShape,
                                                             typename DefaultGemm::Mma::Operator,
                                                             DefaultGemm::kPartitionsK,
                                                             OutputOp,
                                                             OutputOp::kCount>::Epilogue;
                    cutlass::layout::AffineRankN<2> layout({0, 1});
                    auto dst_ptr = p.grad_bias1_ptr + key_start;
                    typename Epilogue::OutputTileIterator output_iter(
                        {layout},
                        dst_ptr,
                        {num_queries_in_block, num_keys_in_block},
                        (int)thread_id);
                    Epilogue epilogue(shared_storage.gradB_epilogue(),
                                      (int)thread_id,
                                      (int)warp_id,
                                      (int)lane_id);
                    epilogue(OutputOp(1), output_iter, accum);
                }

                if (broadcast_2::kEnable && p.grad_bias2_ptr) {
                    if (broadcast_1::kEnable) { __syncthreads(); }
                    using Epilogue =
                        typename BiasGradEpilogue<ArchTag,
                                                  typename MatmulDOIVJ::ThreadblockShape,
                                                  typename DefaultGemm::Mma::Operator,
                                                  DefaultGemm::kPartitionsK,
                                                  OutputOp,
                                                  OutputOp::kCount>::Epilogue;
                    typename Epilogue::OutputTileIterator::Params params{p.num_keys};
                    auto dst_ptr = p.grad_bias2_ptr + query_start * p.num_keys + key_start;
                    typename Epilogue::OutputTileIterator output_iter(
                        params, dst_ptr, {num_queries_in_block, num_keys_in_block}, (int)thread_id);
                    Epilogue epilogue(shared_storage.gradB_epilogue(),
                                      (int)thread_id,
                                      (int)warp_id,
                                      (int)lane_id);
                    epilogue(OutputOp(1), output_iter, accum);
                }

                accum = accum * scale;

                __syncthreads();
                if (!MatmulGradK::DefaultMmaFromSmem::kIsTransposedA) {
                    auto tmpT = shared_storage.tmpT_shared_storage().accum_ref();
                    // attn <- attn_T.T
                    LambdaIterator::iterateRows(
                        lane_offset,
                        [&](int accum_m) {},
                        [&](int accum_m, int accum_n, int idx) {
                            tmpT.at({accum_n, accum_m}) = scalar_t(accum[idx]);
                        },
                        [&](int accum_m) {});
                }
            }

            MatmulDOIVJ::B2bGemm::accumToSmem(
                shared_storage.tmp_shared_storage(), accum, lane_id, output_tile_coords);
            __syncthreads();
        }
        p.head_dim = warp_uniform(p.head_dim);
        p.k_strideM = warp_uniform(p.k_strideM);
        rematerializeThreadIds();
        /////////////////////////////////////////////////////////////////////////////////////////////////
        // GradQ matmul
        //
        // grad_q[i_start:i_end] += tmp @ k_j
        /////////////////////////////////////////////////////////////////////////////////////////////////
        // Skip the loop & associated branches if we know at compile time the number
        // of iterations
        constexpr bool kSingleIterationGradQ = kMaxK <= MatmulGradQ::ThreadblockShape::kN;
        for (int col = 0; col < (kSingleIterationGradQ ? 1 : p.head_dim);
             col += MatmulGradQ::ThreadblockShape::kN) {
            using Mma = typename MatmulGradQ::Mma;
            using AccumTileGmem = typename MatmulGradQ::AccumTileGmem;

            cutlass::gemm::GemmCoord problem_size(
                num_queries_in_block,
                false ? MatmulGradQ::ThreadblockShape::kN : p.head_dim - col,
                num_keys_in_block);

            // k_j
            typename Mma::IteratorB iterator_B({int32_t(p.k_strideM)},
                                               p.key_ptr + key_start * p.k_strideM + col,
                                               {problem_size.k(), problem_size.n()},
                                               thread_id,
                                               no_offset);

            auto a = shared_storage.tmp_shared_storage().accum_ref();
            Mma mma(shared_storage.mm_gradQ(),
                    shared_storage.tmp_shared_storage(),
                    thread_id,
                    warp_id,
                    lane_id,
                    problem_size.k());

            typename Mma::FragmentC accum;

            bool isFirst = key_start == 0;
            int col_id = col / MatmulGradQ::ThreadblockShape::kN;
            int num_cols =
                kSingleIterationGradQ ? 1 : ceil_div(p.head_dim, MatmulGradQ::ThreadblockShape::kN);
            int storage_id = (col_id + query_start / kBlockSizeI * num_cols);
            AccumTileGmem gmem_tile{p.workspace_gq + storage_id * AccumTileGmem::kElementsStored};
            if (isFirst || !kNeedsAccumGradQ) {
                accum.clear();
            } else {
                gmem_tile.load(accum, thread_id);
            }

            auto gemm_k_iterations = (problem_size.k() + Mma::Shape::kK - 1) / Mma::Shape::kK;

            // Compute threadblock-scoped matrix multiply-add
            __syncthreads();
            mma.set_prologue_done(kPrologueGQ);
            mma(gemm_k_iterations, accum, iterator_B, accum);
            __syncthreads();
            bool isLastColumn = kSingleIterationGradQ ||
                                (col + MatmulGradQ::ThreadblockShape::kN >= p.head_dim);
            if (kPrologueGQ && !isLastColumn) {
                prologueGradQ(col + MatmulGradQ::ThreadblockShape::kN);
            }

            // Output results
            int32_t next_query, next_key;
            incrIteration(p, p.num_queries, key_start, next_query, next_key);
            bool isLast = next_query > query_start || next_key >= p.num_keys;
            if (kNeedsAccumGradQ && !isLast) {
                gmem_tile.store(accum, thread_id);
            } else {
                typename MatmulGradQ::OutputTileIterator output_it(
                    typename MatmulGradQ::OutputTileIterator::Params{p.gQ_strideM()},
                    p.grad_query_ptr + query_start * p.gQ_strideM() + col,
                    {problem_size.m(), problem_size.n()},
                    thread_id);
                accumulateInGmem<MatmulGradQ>(isLastColumn
                                                  ? shared_storage.gradQ_epilogue_lastIter()
                                                  : shared_storage.gradQ_epilogue(),
                                              accum,
                                              output_it,
                                              isFirst || kNeedsAccumGradQ,
                                              warp_id,
                                              lane_id);
            }
        }
        /////////////////////////////////////////////////////////////////////////////////////////////////
        // GradK matmul
        //
        // grad_k[i_start:i_end] += tmp.transpose(-2, -1) @ q_i
        /////////////////////////////////////////////////////////////////////////////////////////////////
        rematerializeThreadIds();

        constexpr bool kSingleIterationGradK = kMaxK <= MatmulGradK::ThreadblockShape::kN;
        for (int col = 0; col < (kSingleIterationGradK ? 1 : p.head_dim);
             col += MatmulGradK::ThreadblockShape::kN) {
            using Mma = typename MatmulGradK::Mma;
            using AccumTileGmem = typename MatmulGradQ::AccumTileGmem;

            cutlass::gemm::GemmCoord problem_size(
                num_keys_in_block,
                false ? MatmulGradK::ThreadblockShape::kN : p.head_dim - col,
                num_queries_in_block);
            auto createEpilogueIter = [&]() {
                return typename MatmulGradK::OutputTileIterator(
                    typename MatmulGradK::OutputTileIterator::Params{p.gK_strideM()},
                    p.grad_key_ptr + key_start * p.gK_strideM() + col,
                    {num_keys_in_block,
                     false ? MatmulGradK::ThreadblockShape::kN : p.head_dim - col},
                    thread_id);
            };

            // q_i
            typename Mma::IteratorB iterator_B({int32_t(p.q_strideM)},
                                               p.query_ptr + query_start * p.q_strideM + col,
                                               {problem_size.k(), problem_size.n()},
                                               thread_id,
                                               no_offset);

            auto getTmp = [&](int) { return &shared_storage.tmp_shared_storage(); };
            auto getTmpT = [&](int) { return &shared_storage.tmpT_shared_storage(); };
            // this is basically:
            // opA = kIsTransposedA ? getTmp() : getTmpT();
            bool constexpr kIsTransposedA = MatmulGradK::DefaultMmaFromSmem::kIsTransposedA;
            auto& opA =
                *call_conditional<kIsTransposedA, decltype(getTmp), decltype(getTmpT)>::apply(
                    getTmp, getTmpT, 0);
            Mma mma(shared_storage.mm_gradK(), opA, thread_id, warp_id, lane_id, problem_size.k());

            int storage_id = col / MatmulGradK::ThreadblockShape::kN;
            AccumTileGmem gmem_tile{p.workspace_gk + storage_id * AccumTileGmem::kElementsStored};
            if (!kOutputInRF) {
                if (isFirstQuery || !kNeedsAccumGradK) {
                    output_frags.gradK.clear();
                } else {
                    gmem_tile.load(output_frags.gradK, thread_id);
                }
            }
            mma.set_prologue_done(kPrologueGK);

            auto gemm_k_iterations = (problem_size.k() + Mma::Shape::kK - 1) / Mma::Shape::kK;

            // Compute threadblock-scoped matrix multiply-add
            __syncthreads();

            mma(gemm_k_iterations, output_frags.gradK, iterator_B, output_frags.gradK);
            __syncthreads();
            bool isLastColumn = kSingleIterationGradK ||
                                col + MatmulGradK::ThreadblockShape::kN >= p.head_dim;
            if (kPrologueGK && !isLastColumn) {
                prologueGradK(col + MatmulGradK::ThreadblockShape::kN);
            }

            if (kPrologueQK && isLastColumn) {
                int32_t next_query, next_key;
                incrIteration(p, query_start, key_start, next_query, next_key);
                DISPATCH_BOOL(next_key != key_start, kForceReloadK, ([&]() {
                                  prologueQkNextIteration<kForceReloadK>(
                                      shared_storage, p, next_query, next_key, warp_id, lane_id);
                              }));
            }

            // Output results
            if (!kOutputInRF) {
                if (kNeedsAccumGradK && !isLastQuery) {
                    gmem_tile.store(output_frags.gradK, thread_id);
                } else {
                    accumulateInGmem<MatmulGradK>(isLastColumn
                                                      ? shared_storage.gradK_epilogue_final()
                                                      : shared_storage.gradK_epilogue(),
                                                  output_frags.gradK,
                                                  createEpilogueIter(),
                                                  isFirstQuery || kNeedsAccumGradK,
                                                  warp_id,
                                                  lane_id);
                    __syncthreads();
                }
            }
        }
    }

    static CUTLASS_DEVICE int32_t getQueryStart(Params const& p, int32_t key_start) { return 0; };

    static CUTLASS_DEVICE void incrIteration(Params const& p,
                                             int32_t query_start,
                                             int32_t key_start,
                                             int32_t& next_query,
                                             int32_t& next_key)
    {
        next_query = query_start + kBlockSizeI;
        next_key = key_start;
        if (next_query >= p.num_queries) {
            next_key = key_start + kBlockSizeJ;
            next_query = getQueryStart(p, next_key);
        }
    }

    template <bool kForceReloadK>
    static CUTLASS_DEVICE void prologueQkNextIteration(SharedStorage& shared_storage,
                                                       Params const& p,
                                                       int32_t query_start,
                                                       int32_t key_start,
                                                       uint8_t warp_id,
                                                       uint8_t lane_id)
    {
        if (query_start >= p.num_queries || key_start >= p.num_keys) { return; }

        static constexpr bool kReloadK = kForceReloadK || !MatmulQK::Mma::kSmemContainsEntireMat;
        int thread_id = 32 * warp_id + lane_id;
        typename MatmulQK::Mma::IteratorA iterator_A({int32_t(p.k_strideM)},
                                                     p.key_ptr + key_start * p.k_strideM,
                                                     {p.num_keys - key_start, p.head_dim},
                                                     thread_id,
                                                     cutlass::MatrixCoord{0, 0});

        typename MatmulQK::Mma::IteratorB iterator_B({int32_t(p.q_strideM)},
                                                     p.query_ptr + query_start * p.q_strideM,
                                                     {p.head_dim, p.num_queries - query_start},
                                                     thread_id,
                                                     cutlass::MatrixCoord{0, 0});

        MatmulQK::Mma::prologue<kReloadK, true>(shared_storage.mm_qk_k(),
                                                shared_storage.mm_qk_q(),
                                                iterator_A,
                                                iterator_B,
                                                thread_id,
                                                p.head_dim);
    }

    template <bool skipBoundsChecks>
    static CUTLASS_DEVICE void writeFragsToGmem(SharedStorage& shared_storage,
                                                OutputFragments& output_frags,
                                                Params const& p,
                                                int32_t key_start,
                                                uint8_t warp_id,
                                                uint8_t lane_id)
    {
        uint16_t thread_id = 32 * warp_id + lane_id;
        int32_t num_keys_in_block =
            skipBoundsChecks
                ? MatmulQK::Mma::Shape::kM
                : cutlass::fast_min((int32_t)MatmulQK::Mma::Shape::kM, p.num_keys - key_start);
        typename MatmulGradV::OutputTileIterator outputV_it(
            typename MatmulGradV::OutputTileIterator::Params{p.gV_strideM()},
            p.grad_value_ptr + key_start * p.gV_strideM(),
            {num_keys_in_block, p.head_dim_value},
            thread_id);
        accumulateInGmem<MatmulGradV>(shared_storage.gradV_epilogue_final(),
                                      output_frags.gradV,
                                      outputV_it,
                                      true,
                                      warp_id,
                                      lane_id);

        typename MatmulGradK::OutputTileIterator outputK_it(
            typename MatmulGradK::OutputTileIterator::Params{p.gK_strideM()},
            p.grad_key_ptr + key_start * p.gK_strideM(),
            {num_keys_in_block, false ? MatmulGradK::ThreadblockShape::kN : p.head_dim},
            thread_id);
        accumulateInGmem<MatmulGradK>(shared_storage.gradK_epilogue_final(),
                                      output_frags.gradK,
                                      outputK_it,
                                      true,
                                      warp_id,
                                      lane_id);
    }

    template <typename MatmulT>
    static CUTLASS_DEVICE void accumulateInGmem(
        typename MatmulT::DefaultEpilogue::SharedStorage& epilogue_smem,
        typename MatmulT::Mma::FragmentC const& accum,
        typename MatmulT::OutputTileIterator output_it,
        bool first,
        uint8_t warp_id,
        uint8_t lane_id)
    {
        using DefaultEpilogue = typename MatmulT::DefaultEpilogue;
        using DefaultOutputOp = typename MatmulT::DefaultOutputOp;
        using Mma = typename MatmulT::Mma;
        int thread_id = 32 * warp_id + lane_id;
        DISPATCH_BOOL(
            first, kIsFirst, ([&]() {
                static constexpr auto ScaleType =
                    kIsFirst ? cutlass::epilogue::thread::ScaleType::Nothing
                             : cutlass::epilogue::thread::ScaleType::NoBetaScaling;
                using EpilogueOutputOp = typename cutlass::epilogue::thread::LinearCombination<
                    typename DefaultOutputOp::ElementOutput,
                    DefaultOutputOp::kCount,
                    typename DefaultOutputOp::ElementAccumulator,
                    typename DefaultOutputOp::ElementCompute,
                    ScaleType>;
                using Epilogue = typename cutlass::epilogue::threadblock::EpiloguePipelined<
                    typename DefaultEpilogue::Shape,
                    typename Mma::Operator,
                    DefaultEpilogue::kPartitionsK,
                    typename MatmulT::OutputTileIterator,
                    typename DefaultEpilogue::AccumulatorFragmentIterator,
                    typename DefaultEpilogue::WarpTileIterator,
                    typename DefaultEpilogue::SharedLoadIterator,
                    EpilogueOutputOp,
                    typename DefaultEpilogue::Padding,
                    DefaultEpilogue::kFragmentsPerIteration,
                    true  // IterationsUnroll
                    >;
                EpilogueOutputOp rescale({1, 1});
                Epilogue epilogue(epilogue_smem, thread_id, warp_id, lane_id);
                epilogue(rescale, output_it, accum, output_it);
            }));
    }

    template <int kElementsPerAccess>
    static CUTLASS_DEVICE void computeDelta(Params const& p,
                                            int32_t query_start,
                                            uint8_t warp_id,
                                            uint8_t lane_id)
    {
        // Each thread computes one value for Delta
        // Depending on warp configuration, we might have multiple
        // threads of the same warp working on the same row
        using AccessType = cutlass::Array<scalar_t, kElementsPerAccess>;
        static_assert(kNumThreads >= kBlockSizeI, "");
        static constexpr int kNumThreadsPerLine = kNumThreads / kBlockSizeI;
        int16_t thread_id = 32 * warp_id + lane_id;

        int16_t laneFirstCol = kElementsPerAccess * (lane_id % kNumThreadsPerLine);
        int16_t laneRow = thread_id / kNumThreadsPerLine;
        bool rowPred = (query_start + laneRow) < p.num_queries;
        bool pred = rowPred;

        // on windows, previous syntax __restrict__ AccessType*
        // resulted in error: "restrict" is not allowed
        const AccessType* __restrict__ grad_output_ptr = reinterpret_cast<const AccessType*>(
            p.grad_output_ptr + (query_start + laneRow) * p.gO_strideM + laneFirstCol);
        const AccessType* __restrict__ output_ptr = reinterpret_cast<const AccessType*>(
            p.output_ptr + (query_start + laneRow) * p.o_strideM() + laneFirstCol);

        static constexpr int64_t kMaxIters = kMaxK / (kElementsPerAccess * kNumThreadsPerLine);
        constexpr int kPipelineStages = 2;
        accum_t delta_value = accum_t(0);
        using GlobalLoad = cutlass::arch::global_load<AccessType, sizeof(AccessType)>;
        AccessType frag_grad_output[kPipelineStages];
        AccessType frag_output[kPipelineStages];

        auto loadAndIncrement = [&](int ld_pos, bool is_valid) {
            frag_grad_output[ld_pos].clear();
            frag_output[ld_pos].clear();
            GlobalLoad(frag_grad_output[ld_pos], grad_output_ptr, is_valid);
            GlobalLoad(frag_output[ld_pos], output_ptr, is_valid);
            grad_output_ptr += kNumThreadsPerLine;
            output_ptr += kNumThreadsPerLine;
        };

        CUTLASS_PRAGMA_UNROLL
        for (int iter = 0; iter < kPipelineStages - 1; ++iter) {
            int ld_pos = iter % kPipelineStages;
            pred = pred && (laneFirstCol + iter * kElementsPerAccess * kNumThreadsPerLine) <
                               p.head_dim_value;
            loadAndIncrement(ld_pos, pred);
        }
        auto columnIteration = [&](int iter) {
            // Load for next iter
            int ld_pos = (iter + kPipelineStages - 1) % kPipelineStages;
            pred = pred && (laneFirstCol + (iter + kPipelineStages - 1) * kElementsPerAccess *
                                               kNumThreadsPerLine) < p.head_dim_value;
            loadAndIncrement(ld_pos, pred);
            CUTLASS_PRAGMA_UNROLL
            for (int i = 0; i < AccessType::kElements; ++i) {
                delta_value += accum_t(frag_output[iter % kPipelineStages][i]) *
                               accum_t(frag_grad_output[iter % kPipelineStages][i]);
            }
        };

        // If we have a small lower-bound for K, we can unroll the loop
        if (kMaxK <= 256) {
            CUTLASS_PRAGMA_UNROLL
            for (int iter = 0; iter < kMaxIters; ++iter) { columnIteration(iter); }
        } else {
            int num_iters = ceil_div(p.head_dim_value, kElementsPerAccess * kNumThreadsPerLine) *
                            (kElementsPerAccess * kNumThreadsPerLine);
            for (int iter = 0; iter < num_iters; ++iter) { columnIteration(iter); }
        }

        // Reduce between workers
        static_assert(kNumThreadsPerLine == 1 || kNumThreadsPerLine == 2 || kNumThreadsPerLine == 4,
                      "");
        CUTLASS_PRAGMA_UNROLL
        for (int i = 1; i < kNumThreadsPerLine; i *= 2) {
            delta_value = delta_value + __shfl_xor_sync(0xffffffff, delta_value, i);
        }

        // Store in gmem
        if (rowPred) { p.delta_ptr[query_start + laneRow] = delta_value; }
    }
};

template <typename AK>
__global__ void __launch_bounds__(AK::kNumThreads, AK::kMinBlocksPerSm)
    attention_kernel_backward_batched_impl(typename AK::Params p)
{
    if (!p.advance_to_block()) { return; }
    AK::attention_kernel(p);
}

template <typename AK>
__global__ void __launch_bounds__(AK::kNumThreads, AK::kMinBlocksPerSm)
    attention_kernel_backward_batched(typename AK::Params params);

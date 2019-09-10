/******************************************************************************

   Have to keep this as a separate file, so it won't be linked if unused.

   BC7 - If entire block has the same alpha value, then it's best when that alpha is equal to 255,
      because in that mode, RGB will have more precision.

/******************************************************************************/
#include "stdafx.h"

#include "../../../../ThirdPartyLibs/begin.h"

#define BC_LIB_DIRECTX  1
#define BC_LIB_ISPC     2 // much faster than BC_LIB_DIRECTX
#define BC_LIB_BC7ENC16 3 // fast but alpha channel quality is bad

#define BC_ENC BC_LIB_ISPC

#if BC_ENC==BC_LIB_ISPC
   #if (WINDOWS && !ARM) || MAC
      #include "../../../../ThirdPartyLibs/BC/ispc_texcomp/ispc_texcomp.h" // Windows and Mac link against precompiled lib's generated by Intel Compiler, so all we need is a header
   #else // other platforms need source
      #include "../../../../ThirdPartyLibs/BC/ispc_texcomp/ispc_texcomp.cpp"
      #pragma warning(push)
      #pragma warning(disable:4715) // not all control paths return a value
      #include "../../../../ThirdPartyLibs/BC/kernel.ispc.cpp"
      #pragma warning(pop)
   #endif
#elif BC_ENC==BC_LIB_DIRECTX
   #include "../../../../ThirdPartyLibs/DirectXMath/include.h"
   #include "../../../../ThirdPartyLibs/DirectXTex/BC.h"
   #include "../../../../ThirdPartyLibs/DirectXTex/BC6HBC7.cpp"
#elif BC_ENC==BC_LIB_BC7ENC16
   #include "../../../../ThirdPartyLibs/bc7enc16/bc7enc16.c"
#endif

#include "../../../../ThirdPartyLibs/end.h"

namespace EE{
/******************************************************************************/
static struct BCThreads
{
   Threads  threads;
   Bool     initialized;
   SyncLock lock;

   void init()
   {
      if(!initialized)
      {
         SyncLocker locker(lock); if(!initialized)
         {
         #if BC_ENC==BC_LIB_BC7ENC16
            bc7enc16_compress_block_init();
         #endif
            threads.create(false, Cpu.threads()-1); // -1 because we will do processing on the caller thread too
            initialized=true; // enable at the end
         }
      }
   }
}BC;
/******************************************************************************/
struct Data
{
#if BC_ENC==BC_LIB_ISPC
   union
   {
      bc6h_enc_settings bc6_settings;
      bc7_enc_settings  bc7_settings;
   };
#elif BC_ENC==BC_LIB_BC7ENC16
   bc7enc16_compress_block_params bc7_settings;
#endif
 C Image *src;
   Image *dest;
   Bool   bc6;
   Int    thread_blocks, threads;

   Data(Image &dest)
   {
      T.dest=&dest;
      bc6=(dest.hwType()==IMAGE_BC6);
   #if BC_ENC==BC_LIB_ISPC
      if(bc6)GetProfile_bc6h_basic(&bc6_settings);else
      {
      #if 0 // 3x slower and only small quality difference
         GetProfile_alpha_slow(&bc7_settings);
      #else
         GetProfile_alpha_basic(&bc7_settings);
      #endif
      }
   #elif BC_ENC==BC_LIB_BC7ENC16
      bc7enc16_compress_block_params_init(&bc7_settings);
      bc7_settings.m_uber_level=BC7ENC16_MAX_UBER_LEVEL;
      bc7_settings.m_mode1_partition_estimation_filterbank=false;
      if()bc7enc16_compress_block_params_init_linear_weights(&bc7_settings);
   #endif
   }
   void init(C Image &src)
   {
      T.src=&src;
      Int total_blocks=src.lh()/4;
      threads=Min(total_blocks, BC.threads.threads1()); // +1 because we will do processing on the caller thread too
      thread_blocks=total_blocks/threads;
   }
};
/******************************************************************************/
static void CompressBC67Block(IntPtr elm_index, Data &data, Int thread_index)
{
   Int block_start=elm_index*data.thread_blocks, y_start=block_start*4;
#if BC_ENC==BC_LIB_ISPC
   rgba_surface surf;
   surf.ptr   =ConstCast(data.src->data()+y_start*data.src->pitch());
   surf.stride=data.src->pitch();
   surf.width =data.src->lw   ();
   surf.height=((elm_index==data.threads-1) ? data.src->lh()-y_start : data.thread_blocks*4); // last thread must process all remaining blocks
   if(data.bc6)CompressBlocksBC6H(&surf, data.dest->data() + block_start*data.dest->pitch(), &data.bc6_settings);
   else        CompressBlocksBC7 (&surf, data.dest->data() + block_start*data.dest->pitch(), &data.bc7_settings);
#elif BC_ENC==BC_LIB_DIRECTX
   Int blocks_x=data.src->lw()/4,
       blocks_y=((elm_index==data.threads-1) ? (data.src->lh()-y_start)/4 : data.thread_blocks); // last thread must process all remaining blocks
   REPD(by, blocks_y)
   REPD(bx, blocks_x)
   {
      DirectX::XMVECTOR dx_rgba[4][4]; ASSERT(SIZE(DirectX::XMVECTOR)==SIZE(Vec4));
      Int px=bx*4, py=by*4, // pixel
          xo[4], yo[4];
      REP(4)
      {
         xo[i]=px+i;
         yo[i]=py+i+y_start;
      }
      data.src->gather((Vec4*)&dx_rgba[0][0], xo, Elms(xo), yo, Elms(yo));
      if(data.bc6)DirectX::D3DXEncodeBC6HU(data.dest->data() + bx*16 + (by+block_start)*data.dest->pitch(), &dx_rgba[0][0], 0);
      else        DirectX::D3DXEncodeBC7  (data.dest->data() + bx*16 + (by+block_start)*data.dest->pitch(), &dx_rgba[0][0], 0);
   }
#elif BC_ENC==BC_LIB_BC7ENC16
   Int blocks_x=data.src->lw()/4,
       blocks_y=((elm_index==data.threads-1) ? (data.src->lh()-y_start)/4 : data.thread_blocks); // last thread must process all remaining blocks
   REPD(by, blocks_y)
   REPD(bx, blocks_x)
   {
      Color color[4][4];
      Int px=bx*4, py=by*4, // pixel
          xo[4], yo[4];
      REP(4)
      {
         xo[i]=px+i;
         yo[i]=py+i+y_start;
      }
      data.src->gather(&color[0][0], xo, Elms(xo), yo, Elms(yo));
      if(data.bc6)..
      else        bc7enc16_compress_block(data.dest->data() + bx*16 + (by+block_start)*data.dest->pitch(), &color[0][0], &data.bc7_settings);
   }
#endif
}
/******************************************************************************/
Bool _CompressBC67(C Image &src, Image &dest)
{
   if(dest.hwType()==IMAGE_BC6 || dest.hwType()==IMAGE_BC7 || dest.hwType()==IMAGE_BC7_SRGB)
   {
      BC.init(); Data data(dest);
      Int src_faces1=src.faces()-1;
      Image temp; // define outside loop to avoid overhead
      REPD(mip, Min(src.mipMaps(), dest.mipMaps()))
      {
         Int dest_mip_hwW=PaddedWidth (dest.hwW(), dest.hwH(), mip, dest.hwType()),
             dest_mip_hwH=PaddedHeight(dest.hwW(), dest.hwH(), mip, dest.hwType());
         // to directly read from 'src', we need to match requirements for compressor, which needs:
         Bool read_from_src=((data.bc6 ? src.hwType()==IMAGE_F16_4 : (src.hwType()==IMAGE_R8G8B8A8 || src.hwType()==IMAGE_R8G8B8A8_SRGB)) // IMAGE_F16_4 for BC6 / IMAGE_R8G8B8A8 for BC7
                         && PaddedWidth (src.hwW(), src.hwH(), mip, src.hwType())==dest_mip_hwW   // src mip width  must be exactly the same as dest mip width
                         && PaddedHeight(src.hwW(), src.hwH(), mip, src.hwType())==dest_mip_hwH); // src mip height must be exactly the same as dest mip height
       C Image &s=(read_from_src ? src : temp);
         REPD(face, dest.faces())
         {
            if(!read_from_src)
            {
               if(!src.extractNonCompressedMipMapNoStretch(temp, dest_mip_hwW, dest_mip_hwH, 1, mip, (DIR_ENUM)Min(face, src_faces1), true))return false;
               if(data.bc6){if(temp.hwType()!=IMAGE_F16_4                                         )if(!temp.copyTry(temp, -1, -1, -1,                                     IMAGE_F16_4   ))return false;}
               else        {if(temp.hwType()!=IMAGE_R8G8B8A8 && temp.hwType()!=IMAGE_R8G8B8A8_SRGB)if(!temp.copyTry(temp, -1, -1, -1, dest.sRGB() ? IMAGE_R8G8B8A8_SRGB : IMAGE_R8G8B8A8))return false;}
            }else
            if(! src.lockRead(            mip, (DIR_ENUM)Min(face, src_faces1)))                                return false; // we have to lock only for 'src' because 'temp' is 1mip-1face-SOFT and doesn't need locking
            if(!dest.lock    (LOCK_WRITE, mip, (DIR_ENUM)    face             )){if(read_from_src)src.unlock(); return false;}

            data.init(s); // !! call after 'BC.init' !!
            BC.threads.process1(data.threads, CompressBC67Block, data, INT_MAX); // use all available threads, including this one

                            dest.unlock();
            if(read_from_src)src.unlock();
         }
      }
      return true;
   }
   return false;
}
/******************************************************************************/
}
/******************************************************************************/

/*
Copyright (c) 2003-2010 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <OpenColorIO/OpenColorIO.h>

#include "AllocationOp.h"
#include "GpuShaderUtils.h"
#include "HashUtils.h"
#include "Lut3DOp.h"
#include "OpBuilders.h"
#include "Processor.h"
#include "ScanlineHelper.h"

#include <cstring>
#include <sstream>

#include <iostream>

OCIO_NAMESPACE_ENTER
{
    //////////////////////////////////////////////////////////////////////////
    
    
    ProcessorRcPtr Processor::Create()
    {
        return ProcessorRcPtr(new Processor(), &deleter);
    }
    
    void Processor::deleter(Processor* c)
    {
        delete c;
    }
    
    Processor::Processor()
    : m_impl(new Processor::Impl)
    {
    }
    
    Processor::~Processor()
    {
        delete m_impl;
        m_impl = NULL;
    }
    
    bool Processor::isNoOp() const
    {
        return getImpl()->isNoOp();
    }
    
    bool Processor::hasChannelCrosstalk() const
    {
        return getImpl()->hasChannelCrosstalk();
    }
    
    void Processor::apply(ImageDesc& img) const
    {
        getImpl()->apply(img);
    }
    void Processor::applyRGB(float * pixel) const
    {
        getImpl()->applyRGB(pixel);
    }
    
    void Processor::applyRGBA(float * pixel) const
    {
        getImpl()->applyRGBA(pixel);
    }
    
    const char * Processor::getCpuCacheID() const
    {
        return getImpl()->getCpuCacheID();
    }
    
    const char * Processor::getGpuShaderText(const GpuShaderDesc & shaderDesc) const
    {
        return getImpl()->getGpuShaderText(shaderDesc);
    }
    
    const char * Processor::getGpuShaderTextCacheID(const GpuShaderDesc & shaderDesc) const
    {
        return getImpl()->getGpuShaderTextCacheID(shaderDesc);
    }
    
    void Processor::getGpuLut3D(float* lut3d, const GpuShaderDesc & shaderDesc) const
    {
        return getImpl()->getGpuLut3D(lut3d, shaderDesc);
    }
    
    const char * Processor::getGpuLut3DCacheID(const GpuShaderDesc & shaderDesc) const
    {
        return getImpl()->getGpuLut3DCacheID(shaderDesc);
    }
    
    
    
    //////////////////////////////////////////////////////////////////////////
    
    
    
    namespace
    {
        void WriteShaderHeader(std::ostream & shader,
                               const std::string & pixelName,
                               const GpuShaderDesc & shaderDesc)
        {
            if(!shader) return;
            
            std::string lut3dName = "lut3d";
            
            shader << "\n// Generated by OpenColorIO\n\n";
            
            GpuLanguage lang = shaderDesc.getLanguage();
            
            std::string fcnName = shaderDesc.getFunctionName();
            
            if(lang == GPU_LANGUAGE_CG)
            {
                shader << "half4 " << fcnName << "(in half4 inPixel," << "\n";
                shader << "    const uniform sampler3D " << lut3dName << ") \n";
            }
            else if(lang == GPU_LANGUAGE_GLSL_1_0)
            {
                shader << "vec4 " << fcnName << "(vec4 inPixel, \n";
                shader << "    sampler3D " << lut3dName << ") \n";
            }
            else if(lang == GPU_LANGUAGE_GLSL_1_3)
            {
                shader << "vec4 " << fcnName << "(in vec4 inPixel, \n";
                shader << "    const uniform sampler3D " << lut3dName << ") \n";
            }
            else throw Exception("Unsupported shader language.");
            
            shader << "{" << "\n";
            
            if(lang == GPU_LANGUAGE_CG)
            {
                shader << "half4 " << pixelName << " = inPixel; \n";
            }
            else if(lang == GPU_LANGUAGE_GLSL_1_0 || lang == GPU_LANGUAGE_GLSL_1_3)
            {
                shader << "vec4 " << pixelName << " = inPixel; \n";
            }
            else throw Exception("Unsupported shader language.");
        }
        
        
        void WriteShaderFooter(std::ostream & shader,
                               const std::string & pixelName,
                               const GpuShaderDesc & /*shaderDesc*/)
        {
            shader << "return " << pixelName << ";\n";
            shader << "}" << "\n\n";
        }
        
        // Find the minimal index range in the opVec that does not support
        // shader text generation.  The endIndex *is* inclusive.
        // 
        // I.e., if the entire opVec does not support GPUShaders, the
        // result will be startIndex = 0, endIndex = opVec.size() - 1
        // 
        // If the entire opVec supports GPU generation, both the
        // startIndex and endIndex will equal -1
        
        void GetGpuUnsupportedIndexRange(int * startIndex, int * endIndex,
                                         const OpRcPtrVec & opVec)
        {
            int start = -1;
            int end = -1;
            
            for(unsigned int i=0; i<opVec.size(); ++i)
            {
                // We've found a gpu unsupported op.
                // If it's the first, save it as our start.
                // Otherwise, update the end.
                
                if(!opVec[i]->supportsGpuShader())
                {
                    if(start<0)
                    {
                        start = i;
                        end = i;
                    }
                    else end = i;
                }
            }
            
            // Now that we've found a startIndex, walk back until we find
            // one that defines a GpuAllocation. (we can only upload to
            // the gpu at a location are tagged with an allocation)
            
            while(start>0)
            {
                if(opVec[start]->definesAllocation()) break;
                 --start;
            }
            
            if(startIndex) *startIndex = start;
            if(endIndex) *endIndex = end;
        }
        
        AllocationData GetAllocation(int index, const OpRcPtrVec & opVec)
        {
            if(index >=0 && opVec[index]->definesAllocation())
            {
                return opVec[index]->getAllocation();
            }
            
            return AllocationData();
        }
    }
    
    
    //////////////////////////////////////////////////////////////////////////
    
    
    Processor::Impl::Impl()
    { }
    
    Processor::Impl::~Impl()
    { }
    
    bool Processor::Impl::isNoOp() const
    {
        return IsOpVecNoOp(m_cpuOps);
    }
    
    bool Processor::Impl::hasChannelCrosstalk() const
    {
        for(OpRcPtrVec::size_type i=0, size = m_cpuOps.size(); i<size; ++i)
        {
            if(m_cpuOps[i]->hasChannelCrosstalk()) return true;
        }
        
        return false;
    }
    
    void Processor::Impl::apply(ImageDesc& img) const
    {
        if(m_cpuOps.empty()) return;
        
        ScanlineHelper scanlineHelper(img);
        float * rgbaBuffer = 0;
        long numPixels = 0;
        
        while(true)
        {
            scanlineHelper.prepRGBAScanline(&rgbaBuffer, &numPixels);
            if(numPixels == 0) break;
            if(!rgbaBuffer)
                throw Exception("Cannot apply transform; null image.");
            
            for(OpRcPtrVec::size_type i=0, size = m_cpuOps.size(); i<size; ++i)
            {
                m_cpuOps[i]->apply(rgbaBuffer, numPixels);
            }
            
            scanlineHelper.finishRGBAScanline();
        }
    }
    
    void Processor::Impl::applyRGB(float * pixel) const
    {
        if(m_cpuOps.empty()) return;
        
        // We need to allocate a temp array as the pixel must be 4 floats in size
        // (otherwise, sse loads will potentially fail)
        
        float rgbaBuffer[4] = { pixel[0], pixel[1], pixel[2], 0.0f };
        
        for(OpRcPtrVec::size_type i=0, size = m_cpuOps.size(); i<size; ++i)
        {
            m_cpuOps[i]->apply(rgbaBuffer, 1);
        }
        
        pixel[0] = rgbaBuffer[0];
        pixel[1] = rgbaBuffer[1];
        pixel[2] = rgbaBuffer[2];
    }
    
    void Processor::Impl::applyRGBA(float * pixel) const
    {
        for(OpRcPtrVec::size_type i=0, size = m_cpuOps.size(); i<size; ++i)
        {
            m_cpuOps[i]->apply(pixel, 1);
        }
    }
    
    const char * Processor::Impl::getCpuCacheID() const
    {
        AutoMutex lock(m_resultsCacheMutex);
        
        if(!m_cpuCacheID.empty()) return m_cpuCacheID.c_str();
        
        if(m_cpuOps.empty())
        {
            m_cpuCacheID = "<NOOP>";
        }
        else
        {
            std::ostringstream cacheid;
            for(OpRcPtrVec::size_type i=0, size = m_cpuOps.size(); i<size; ++i)
            {
                cacheid << m_cpuOps[i]->getCacheID() << " ";
            }
            std::string fullstr = cacheid.str();
            
            m_cpuCacheID = CacheIDHash(fullstr.c_str(), (int)fullstr.size());
        }
        
        return m_cpuCacheID.c_str();
    }
    
    
    ///////////////////////////////////////////////////////////////////////////
    
    
    
    
    const char * Processor::Impl::getGpuShaderText(const GpuShaderDesc & shaderDesc) const
    {
        AutoMutex lock(m_resultsCacheMutex);
        
        if(m_lastShaderDesc != shaderDesc.getCacheID())
        {
            m_lastShaderDesc = shaderDesc.getCacheID();
            m_shader = "";
            m_shaderCacheID = "";
            m_lut3D.clear();
            m_lut3DCacheID = "";
        }
        
        if(m_shader.empty())
        {
            std::ostringstream shader;
            calcGpuShaderText(shader, shaderDesc);
            m_shader = shader.str();
        }
        
        return m_shader.c_str();
    }
    
    const char * Processor::Impl::getGpuShaderTextCacheID(const GpuShaderDesc & shaderDesc) const
    {
        AutoMutex lock(m_resultsCacheMutex);
        
        if(m_lastShaderDesc != shaderDesc.getCacheID())
        {
            m_lastShaderDesc = shaderDesc.getCacheID();
            m_shader = "";
            m_shaderCacheID = "";
            m_lut3D.clear();
            m_lut3DCacheID = "";
        }
        
        if(m_shader.empty())
        {
            std::ostringstream shader;
            calcGpuShaderText(shader, shaderDesc);
            m_shader = shader.str();
        }
        
        if(m_shaderCacheID.empty())
        {
            m_shaderCacheID = CacheIDHash(m_shader.c_str(), (int)m_shader.size());
        }
        
        return m_shaderCacheID.c_str();
    }
    
    
    const char * Processor::Impl::getGpuLut3DCacheID(const GpuShaderDesc & shaderDesc) const
    {
        AutoMutex lock(m_resultsCacheMutex);
        
        if(m_lastShaderDesc != shaderDesc.getCacheID())
        {
            m_lastShaderDesc = shaderDesc.getCacheID();
            m_shader = "";
            m_shaderCacheID = "";
            m_lut3D.clear();
            m_lut3DCacheID = "";
        }
        
        if(m_lut3DCacheID.empty())
        {
            if(m_gpuOpsCpuLatticeProcess.empty())
            {
                m_lut3DCacheID = "<NULL>";
            }
            else
            {
                std::ostringstream cacheid;
                for(OpRcPtrVec::size_type i=0, size = m_gpuOpsCpuLatticeProcess.size(); i<size; ++i)
                {
                    cacheid << m_gpuOpsCpuLatticeProcess[i]->getCacheID() << " ";
                }
                // Also, add a hash of the shader description
                cacheid << shaderDesc.getCacheID();
                std::string fullstr = cacheid.str();
                m_lut3DCacheID = CacheIDHash(fullstr.c_str(), (int)fullstr.size());
            }
        }
        
        return m_lut3DCacheID.c_str();
    }
    
    void Processor::Impl::getGpuLut3D(float* lut3d, const GpuShaderDesc & shaderDesc) const
    {
        if(!lut3d) return;
        
        AutoMutex lock(m_resultsCacheMutex);
        
        if(m_lastShaderDesc != shaderDesc.getCacheID())
        {
            m_lastShaderDesc = shaderDesc.getCacheID();
            m_shader = "";
            m_shaderCacheID = "";
            m_lut3D.clear();
            m_lut3DCacheID = "";
        }
        
        int lut3DEdgeLen = shaderDesc.getLut3DEdgeLen();
        int lut3DNumPixels = lut3DEdgeLen*lut3DEdgeLen*lut3DEdgeLen;
        
        // Can we write the entire shader using only shader text?
        // If so, the lut3D is not needed so clear it.
        // This is preferable to identity, as it lets people notice if
        // it's accidentally being used.
        if(m_gpuOpsCpuLatticeProcess.empty())
        {
            memset(lut3d, 0, sizeof(float) * 3 * lut3DNumPixels);
            return;
        }
        
        if(m_lut3D.empty())
        {
            // Allocate 3dlut image, RGBA
            m_lut3D.resize(lut3DNumPixels*4);
            GenerateIdentityLut3D(&m_lut3D[0], lut3DEdgeLen, 4, LUT3DORDER_FAST_RED);
            
            // Apply the lattice ops to it
            for(int i=0; i<(int)m_gpuOpsCpuLatticeProcess.size(); ++i)
            {
                m_gpuOpsCpuLatticeProcess[i]->apply(&m_lut3D[0], lut3DNumPixels);
            }
            
            // Convert the RGBA image to an RGB image, in place.
            // Of course, this only works because we're doing it from left to right
            // so old pixels are read before they're written over
            // TODO: is this bad for memory access patterns?
            //       see if this is faster with a 2nd temp float array
            
            for(int i=1; i<lut3DNumPixels; ++i) // skip the 1st pixel, it's ok.
            {
                m_lut3D[3*i+0] = m_lut3D[4*i+0];
                m_lut3D[3*i+1] = m_lut3D[4*i+1];
                m_lut3D[3*i+2] = m_lut3D[4*i+2];
            }
        }
        
        // Copy to the destination
        memcpy(lut3d, &m_lut3D[0], sizeof(float) * 3 * lut3DNumPixels);
    }
    
    
    
    ///////////////////////////////////////////////////////////////////////////
    
    
    
    void Processor::Impl::addColorSpaceConversion(const Config & config,
                                 const ConstContextRcPtr & context,
                                 const ConstColorSpaceRcPtr & srcColorSpace,
                                 const ConstColorSpaceRcPtr & dstColorSpace)
    {
        BuildColorSpaceOps(m_cpuOps, config, context, srcColorSpace, dstColorSpace);
    }
    
    
    void Processor::Impl::addTransform(const Config & config,
                      const ConstContextRcPtr & context,
                      const ConstTransformRcPtr& transform,
                      TransformDirection direction)
    {
        BuildOps(m_cpuOps, config, context, transform, direction);
    }
    
    void Processor::Impl::finalize()
    {
        // GPU Process setup
        {
            //
            // Partition the original, raw opvec into 3 segments for GPU Processing
            //
            // Interior index range does not support the gpu shader.
            // This is used to bound our analytical shader text generation
            // start index and end index are inclusive.
            
            int gpuLut3DOpStartIndex = 0;
            int gpuLut3DOpEndIndex = 0;
            GetGpuUnsupportedIndexRange(&gpuLut3DOpStartIndex,
                                        &gpuLut3DOpEndIndex,
                                        m_cpuOps);
            
            // Write the entire shader using only shader text (3d lut is unused)
            if(gpuLut3DOpStartIndex == -1 && gpuLut3DOpEndIndex == -1)
            {
                for(unsigned int i=0; i<m_cpuOps.size(); ++i)
                {
                    m_gpuOpsHwPreProcess.push_back( m_cpuOps[i]->clone() );
                }
            }
            // Analytical -> 3dlut -> analytical
            else
            {
                // Handle analytical shader block before start index.
                for(int i=0; i<gpuLut3DOpStartIndex; ++i)
                {
                    m_gpuOpsHwPreProcess.push_back( m_cpuOps[i]->clone() );
                }
                
                // Get the GPU Allocation at the cross-over point
                // Create 2 symmetrically canceling allocation ops,
                // where the shader text moves to a nicely allocated LDR
                // (low dynamic range color space), and the lattice processing
                // does the inverse (making the overall operation a no-op
                // color-wise
                
                AllocationData allocation = GetAllocation(gpuLut3DOpStartIndex, m_cpuOps);
                CreateAllocationOps(m_gpuOpsHwPreProcess, allocation, TRANSFORM_DIR_FORWARD);
                CreateAllocationOps(m_gpuOpsCpuLatticeProcess, allocation, TRANSFORM_DIR_INVERSE);
                
                // Handle cpu lattice processing
                for(int i=gpuLut3DOpStartIndex; i<=gpuLut3DOpEndIndex; ++i)
                {
                    m_gpuOpsCpuLatticeProcess.push_back( m_cpuOps[i]->clone() );
                }
                
                // And then handle the gpu post processing
                for(int i=gpuLut3DOpEndIndex+1; i<(int)m_cpuOps.size(); ++i)
                {
                    m_gpuOpsHwPostProcess.push_back( m_cpuOps[i]->clone() );
                }
            }
            
            // TODO: Optimize opvecs
            FinalizeOpVec(m_gpuOpsHwPreProcess);
            FinalizeOpVec(m_gpuOpsCpuLatticeProcess);
            FinalizeOpVec(m_gpuOpsHwPostProcess);
        }
        
        // CPU Process setup
        {
            // TODO: Optimize opvec
            FinalizeOpVec(m_cpuOps);
        }
        
        // TODO: Make this a debug envvar
        /*
        std::cerr << "     ********* CPU OPS ***************" << std::endl;
        std::cerr << GetOpVecInfo(m_cpuOps) << "\n\n";
        
        std::cerr << "     ********* GPU OPS PRE PROCESS ***************" << std::endl;
        std::cerr << GetOpVecInfo(m_gpuOpsHwPreProcess) << "\n\n";
        
        std::cerr << "     ********* GPU OPS LATTICE PROCESS ***************" << std::endl;
        std::cerr << GetOpVecInfo(m_gpuOpsCpuLatticeProcess) << "\n\n";
        
        std::cerr << "     ********* GPU OPS POST PROCESS ***************" << std::endl;
        std::cerr << GetOpVecInfo(m_gpuOpsHwPostProcess) << "\n\n";
        */
    }
    
    void Processor::Impl::calcGpuShaderText(std::ostream & shader,
                                            const GpuShaderDesc & shaderDesc) const
    {
        std::string pixelName = "out_pixel";
        std::string lut3dName = "lut3d";
        
        WriteShaderHeader(shader, pixelName, shaderDesc);
        
        
        for(unsigned int i=0; i<m_gpuOpsHwPreProcess.size(); ++i)
        {
            m_gpuOpsHwPreProcess[i]->writeGpuShader(shader, pixelName, shaderDesc);
        }
        
        if(!m_gpuOpsCpuLatticeProcess.empty())
        {
            // Sample the 3D LUT.
            int lut3DEdgeLen = shaderDesc.getLut3DEdgeLen();
            shader << pixelName << ".rgb = ";
            Write_sampleLut3D_rgb(shader, pixelName,
                                  lut3dName, lut3DEdgeLen,
                                  shaderDesc.getLanguage());
        }
#ifdef __APPLE__
        else
        {
            // Force a no-op sampling of the 3d lut on OSX to work around a segfault.
            int lut3DEdgeLen = shaderDesc.getLut3DEdgeLen();
            shader << "// OSX segfault work-around: Force a no-op sampling of the 3d lut.\n";
            Write_sampleLut3D_rgb(shader, pixelName,
                                  lut3dName, lut3DEdgeLen,
                                  shaderDesc.getLanguage());
        }
#endif // __APPLE__
        for(unsigned int i=0; i<m_gpuOpsHwPostProcess.size(); ++i)
        {
            m_gpuOpsHwPostProcess[i]->writeGpuShader(shader, pixelName, shaderDesc);
        }
        
        WriteShaderFooter(shader, pixelName, shaderDesc);
    }
    
}
OCIO_NAMESPACE_EXIT

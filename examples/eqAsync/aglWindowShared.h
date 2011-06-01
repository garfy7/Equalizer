/* Copyright (c)  2011, Maxim Makhinya <maxmah@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of Eyescale Software GmbH nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#ifndef EQ_EXAMPLE_AGL_WINDOW_SHARED_H
#define EQ_EXAMPLE_AGL_WINDOW_SHARED_H

#include <eq/os.h>

#ifdef AGL

#include <eq/aglWindow.h>

namespace eqAsync
{

/**
 *  Replacing chooseAGLPixelFormat function in eq, since full screen has to be 
 *  set for shared context windows even when it is FBO. 
 */
class AGLWindowShared : public eq::AGLWindow
{
public:
    AGLWindowShared( eq::Window* parent, CGDirectDisplayID displayID = 0 );

    virtual AGLPixelFormat chooseAGLPixelFormat();
private:
    CGDirectDisplayID _cgDisplayID;
};

} // namespace eqAsync

#endif //AGL

#endif //EQ_EXAMPLE_AGL_WINDOW_SHARED_H

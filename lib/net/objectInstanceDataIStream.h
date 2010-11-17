
/* Copyright (c) 2007-2010, Stefan Eilemann <eile@equalizergraphics.com> 
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 2.1 as published
 * by the Free Software Foundation.
 *  
 * This library is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for more
 * details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with this library; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef EQNET_OBJECTINSTANCEDATAISTREAM_H
#define EQNET_OBJECTINSTANCEDATAISTREAM_H

#include "objectDataIStream.h"   // base class

namespace eq
{
namespace net
{
    class Command;

    /**
     * The DataIStream for object instance data.
     */
    class ObjectInstanceDataIStream : public ObjectDataIStream
    {
    public:
        ObjectInstanceDataIStream() {}
        ObjectInstanceDataIStream( const ObjectInstanceDataIStream& from )
                : ObjectDataIStream( from ) {}
        virtual ~ObjectInstanceDataIStream() {}

        virtual Type getType() const { return TYPE_INSTANCE; }

    protected:
        virtual bool getNextBuffer( uint32_t* compressor, uint32_t* nChunks,
                                    const void** chunkData, uint64_t* size );
    };
}
}
#endif //EQNET_OBJECTINSTANCEDATAISTREAM_H

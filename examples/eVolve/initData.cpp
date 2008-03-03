/*
 * Copyright (c) 2006-2008, Stefan Eilemann <eile@equalizergraphics.com> 
 * All rights reserved. 
 *
 * The init data manages static, per-instance application data. In this
 * example, it holds the model file name, and manages the instanciation of the
 * frame data. The instance data is constructed dynamically (due to the use of a
 * string) and cached for further use. The frame data is instanciated seperately
 * for each thread, so that multiple pipe threads on a node can render different
 * frames concurrently.
 */

#include "initData.h"

using namespace eqBase;
using namespace std;

namespace eVolve
{

InitData::InitData()
        : _frameDataID(  EQ_UNDEFINED_UINT32 )
        , _windowSystem( eq::WINDOW_SYSTEM_NONE )
        , _precision( 2 )
        , _brightness( 1.0f )
        , _alpha( 1.0f )
        , _perspective( true )
#ifdef WIN32_VC
        , _filename( "../examples/eVolve/Bucky32x32x32_d.raw" )
#else
        , _filename( "../share/data/Bucky32x32x32_d.raw" )
#endif
{}

InitData::~InitData()
{
    setFrameDataID( EQ_ID_INVALID );
}

void InitData::getInstanceData( eqNet::DataOStream& os )
{
    os << _frameDataID << _windowSystem << _precision << _perspective 
       << _brightness << _alpha << _filename;
}

void InitData::applyInstanceData( eqNet::DataIStream& is )
{
    is >> _frameDataID >> _windowSystem >> _precision >> _perspective
       >> _brightness >> _alpha >> _filename;

    EQASSERT( _frameDataID != EQ_ID_INVALID );
    EQINFO << "New InitData instance" << endl;
}
}

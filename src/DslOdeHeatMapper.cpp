/*
The MIT License

Copyright (c) 2019-2021, Prominence AI, Inc.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in-
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "DslOdeHeatMapper.h"

#define DATE_BUFF_LENGTH 40

namespace DSL
{
    OdeHeatMapper::OdeHeatMapper(const char* name, uint cols, uint rows,
        uint bboxTestPoint, DSL_RGBA_COLOR_PALETTE_PTR pColorPalette)
        : OdeBase(name)
        , m_cols(cols)
        , m_rows(rows)
        , m_gridRectWidth(0)
        , m_gridRectHeight(0)
        , m_bboxTestPoint(bboxTestPoint)
        , m_pColorPalette(pColorPalette)
        , m_heatMap(rows, std::vector<uint64_t> (cols, 0))
        , m_totalOccurrences(0)
        , m_mostOccurrences(0)
    {
        LOG_FUNC();
    }

    OdeHeatMapper::~OdeHeatMapper()
    {
        LOG_FUNC();
    }

    void OdeHeatMapper::HandleOccurrence(NvDsFrameMeta* pFrameMeta, 
        NvDsObjectMeta* pObjectMeta)
    {
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        // one-time initialization of the grid rectangle dimensions
        if (!m_gridRectWidth)
        {
            m_gridRectWidth = pFrameMeta->source_frame_width/m_cols;
            m_gridRectHeight = pFrameMeta->source_frame_height/m_rows;
        }
        
        // get the x,y map coordinates based on the bbox and test-point.
        dsl_coordinate mapCoordinate;
        getCoordinate(pObjectMeta, mapCoordinate);

        // determine the column and row that maps to the x, y coordinates
        uint colPosition(mapCoordinate.x/m_gridRectWidth);
        uint rowPosition(mapCoordinate.y/m_gridRectHeight);

        // increment the running count of occurrences at this poisition
        m_heatMap[rowPosition][colPosition] += 1;
        
        // if the new total for this position is now the greatest  
        if (m_heatMap[rowPosition][colPosition] > m_mostOccurrences)
        {
            m_mostOccurrences = m_heatMap[rowPosition][colPosition];
        }
    }
  
    void OdeHeatMapper::AddDisplayMeta(std::vector<NvDsDisplayMeta*>& displayMetaData)
    {
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        // Iterate through all rows
        for (uint i=0; i < m_rows; i++)
        {
            // and for each row, iterate through all columns.
            for (uint j=0; j < m_cols; j++)
            {
                // if we have at least once occurrence at the current iteration
                if (m_heatMap[i][j] > 1)
                {
                    // Callculate the index into the color palette of size 10 as 
                    // a ratio of occurrences for the current position vs. the 
                    // position with the most occurrences. 
                    
                    // multiply the occurrence for the current position by 10 and 
                    // divide by the most occurrences rouded up or down.
                    m_pColorPalette->SetIndex(
                        std::round((double)m_heatMap[i][j]*10 / (double)(m_mostOccurrences)));
                    
                    DSL_RGBA_RECTANGLE_PTR pRectangle = DSL_RGBA_RECTANGLE_NEW(
                        "", j*m_gridRectWidth, i*m_gridRectHeight, m_gridRectWidth, 
                        m_gridRectHeight, false, m_pColorPalette, true, m_pColorPalette);
                        
                    pRectangle->AddMeta(displayMetaData, NULL);
                }
            }
        }
    }
    
    void OdeHeatMapper::Reset()
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        for (uint i=0; i < m_rows; i++)
        {
            for (uint j=0; j < m_cols; j++)
            {
                m_heatMap[i][j] = 0;
            }
        }
    }
    
    void OdeHeatMapper::Dump()
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        for (auto const& ivec: m_heatMap)
        {
            for (auto const& jvec: ivec)
            {
                std::stringstream ss;
                ss << std::setw(7) << std::setfill(' ') << jvec;
                std::cout << ss.str();
            }
            std::cout << std::endl;
        }
    }

    void OdeHeatMapper::getCoordinate(NvDsObjectMeta* pObjectMeta, 
        dsl_coordinate& mapCoordinate)
    {
        switch (m_bboxTestPoint)
        {
        case DSL_BBOX_POINT_CENTER :
            mapCoordinate.x = round(pObjectMeta->rect_params.left + 
                pObjectMeta->rect_params.width/2);
            mapCoordinate.y = round(pObjectMeta->rect_params.top + 
                pObjectMeta->rect_params.height/2);
            break;
        case DSL_BBOX_POINT_NORTH_WEST :
            mapCoordinate.x = round(pObjectMeta->rect_params.left);
            mapCoordinate.y = round(pObjectMeta->rect_params.top);
            break;
        case DSL_BBOX_POINT_NORTH :
            mapCoordinate.x = round(pObjectMeta->rect_params.left + 
                pObjectMeta->rect_params.width/2);
            mapCoordinate.y = round(pObjectMeta->rect_params.top);
            break;
        case DSL_BBOX_POINT_NORTH_EAST :
            mapCoordinate.x = round(pObjectMeta->rect_params.left + 
                pObjectMeta->rect_params.width);
            mapCoordinate.y = round(pObjectMeta->rect_params.top);
            break;
        case DSL_BBOX_POINT_EAST :
            mapCoordinate.x = round(pObjectMeta->rect_params.left + 
                pObjectMeta->rect_params.width);
            mapCoordinate.y = round(pObjectMeta->rect_params.top + 
                pObjectMeta->rect_params.height/2);
            break;
        case DSL_BBOX_POINT_SOUTH_EAST :
            mapCoordinate.x = round(pObjectMeta->rect_params.left + 
                pObjectMeta->rect_params.width);
            mapCoordinate.y = round(pObjectMeta->rect_params.top + 
                pObjectMeta->rect_params.height);
            break;
        case DSL_BBOX_POINT_SOUTH :
            mapCoordinate.x = round(pObjectMeta->rect_params.left + 
                pObjectMeta->rect_params.width/2);
            mapCoordinate.y = round(pObjectMeta->rect_params.top + 
                pObjectMeta->rect_params.height);
            break;
        case DSL_BBOX_POINT_SOUTH_WEST :
            mapCoordinate.x = round(pObjectMeta->rect_params.left);
            mapCoordinate.y = round(pObjectMeta->rect_params.top + 
                pObjectMeta->rect_params.height);
            break;
        case DSL_BBOX_POINT_WEST :
            mapCoordinate.x = round(pObjectMeta->rect_params.left);
            mapCoordinate.y = round(pObjectMeta->rect_params.top + 
                pObjectMeta->rect_params.height/2);
            break;
        default:
            LOG_ERROR("Invalid DSL_BBOX_POINT = '" << m_bboxTestPoint 
                << "' for Heat-Mapper");
            throw;
        }          
    }
    
}
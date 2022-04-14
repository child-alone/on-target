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

#include "Dsl.h"
#include "DslOdeTrigger.h"
#include "DslOdeAction.h"
#include "DslOdeArea.h"
#include "DslServices.h"

namespace DSL
{

    // Initialize static Event Counter
    uint64_t OdeTrigger::s_eventCount = 0;

    OdeTrigger::OdeTrigger(const char* name, const char* source, 
        uint classId, uint limit)
        : OdeBase(name)
        , m_wName(m_name.begin(), m_name.end())
        , m_source(source)
        , m_sourceId(-1)
        , m_inferId(-1)
        , m_classId(classId)
        , m_triggered(0)
        , m_limit(limit)
        , m_occurrences(0)
        , m_minConfidence(0)
        , m_minWidth(0)
        , m_minHeight(0)
        , m_maxWidth(0)
        , m_maxHeight(0)
        , m_minFrameCountN(1)
        , m_minFrameCountD(1)
        , m_inferDoneOnly(false)
        , m_resetTimeout(0)
        , m_resetTimerId(0)
        , m_interval(0)
        , m_intervalCounter(0)
        , m_skipFrame(false)
        , m_nextAreaIndex(0)
        , m_nextActionIndex(0)
    {
        LOG_FUNC();

        g_mutex_init(&m_propertyMutex);
        g_mutex_init(&m_resetTimerMutex);
    }

    OdeTrigger::~OdeTrigger()
    {
        LOG_FUNC();
        
        RemoveAllActions();
        RemoveAllAreas();
        
        if (m_resetTimerId)
        {
            LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_resetTimerMutex);
            g_source_remove(m_resetTimerId);
        }
        g_mutex_clear(&m_resetTimerMutex);
        g_mutex_clear(&m_propertyMutex);
    }
    
    bool OdeTrigger::AddAction(DSL_BASE_PTR pChild)
    {
        LOG_FUNC();
        
        if (m_pOdeActions.find(pChild->GetName()) != m_pOdeActions.end())
        {
            LOG_ERROR("ODE Area '" << pChild->GetName() 
                << "' is already a child of ODE Trigger'" << GetName() << "'");
            return false;
        }
        
        // increment next index, assign to the Action, and update parent releationship.
        pChild->SetIndex(++m_nextActionIndex);
        pChild->AssignParentName(GetName());

        // Add the shared pointer to child to both Maps, by name and index
        m_pOdeActions[pChild->GetName()] = pChild;
        m_pOdeActionsIndexed[m_nextActionIndex] = pChild;
        
        return true;
    }

    bool OdeTrigger::RemoveAction(DSL_BASE_PTR pChild)
    {
        LOG_FUNC();
        
        if (m_pOdeActions.find(pChild->GetName()) == m_pOdeActions.end())
        {
            LOG_WARN("'" << pChild->GetName() 
                <<"' is not a child of ODE Trigger '" << GetName() << "'");
            return false;
        }
        
        // Erase the child from both maps
        m_pOdeActions.erase(pChild->GetName());
        m_pOdeActionsIndexed.erase(pChild->GetIndex());
        
        // Clear the parent relationship and index
        pChild->ClearParentName();
        pChild->SetIndex(0);
        return true;
    }
    
    void OdeTrigger::RemoveAllActions()
    {
        LOG_FUNC();
        
        for (auto &imap: m_pOdeActions)
        {
            LOG_DEBUG("Removing Action '" << imap.second->GetName() 
                <<"' from Parent '" << GetName() << "'");
            imap.second->ClearParentName();
        }
        m_pOdeActions.clear();
        m_pOdeActionsIndexed.clear();
    }
    
    bool OdeTrigger::AddArea(DSL_BASE_PTR pChild)
    {
        LOG_FUNC();
        
        if (m_pOdeAreas.find(pChild->GetName()) != m_pOdeAreas.end())
        {
            LOG_ERROR("ODE Area '" << pChild->GetName() 
                << "' is already a child of ODE Trigger'" << GetName() << "'");
            return false;
        }
        // increment next index, assign to the Action, and update parent releationship.
        pChild->SetIndex(++m_nextAreaIndex);
        pChild->AssignParentName(GetName());
        
        // Add the shared pointer to child to both Maps, by name and index
        m_pOdeAreas[pChild->GetName()] = pChild;
        m_pOdeAreasIndexed[m_nextAreaIndex] = pChild;
        
        return true;
    }

    bool OdeTrigger::RemoveArea(DSL_BASE_PTR pChild)
    {
        LOG_FUNC();
        
        if (m_pOdeAreas.find(pChild->GetName()) == m_pOdeAreas.end())
        {
            LOG_WARN("'" << pChild->GetName() 
                <<"' is not a child of ODE Trigger '" << GetName() << "'");
            return false;
        }
        
        // Erase the child from both maps
        m_pOdeAreas.erase(pChild->GetName());
        m_pOdeAreasIndexed.erase(pChild->GetIndex());

        // Clear the parent relationship and index
        pChild->ClearParentName();
        pChild->SetIndex(0);
        
        return true;
    }
    
    void OdeTrigger::RemoveAllAreas()
    {
        LOG_FUNC();
        
        for (auto &imap: m_pOdeAreas)
        {
            LOG_DEBUG("Removing Action '" << imap.second->GetName() 
                <<"' from Parent '" << GetName() << "'");
            imap.second->ClearParentName();
        }
        m_pOdeAreas.clear();
        m_pOdeAreasIndexed.clear();
    }
    
    void OdeTrigger::Reset()
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_triggered = 0;

        // iterate through the map of limit-event-listeners calling each
        for(auto const& imap: m_limitEventListeners)
        {
            try
            {
                imap.first(DSL_ODE_TRIGGER_LIMIT_EVENT_COUNT_RESET, 
                    m_limit, imap.second);
            }
            catch(...)
            {
                LOG_ERROR("Exception calling Client Limit-State-Change-Lister");
            }
        }
    }
    
    void OdeTrigger::IncrementAndCheckTriggerCount()
    {
        LOG_FUNC();
        // internal do not lock m_propertyMutex
        
        m_triggered++;
        
        if (m_triggered >= m_limit)
        {
            // iterate through the map of limit-event-listeners calling each
            for(auto const& imap: m_limitEventListeners)
            {
                try
                {
                    imap.first(DSL_ODE_TRIGGER_LIMIT_EVENT_LIMIT_REACHED, 
                        m_limit, imap.second);
                }
                catch(...)
                {
                    LOG_ERROR("Exception calling Client Limit-Event-Lister");
                }
            }
            if (m_resetTimeout)
            {
                m_resetTimerId = g_timeout_add(1000*m_resetTimeout, 
                    TriggerResetTimeoutHandler, this);            
            }
        }
    }

    static int TriggerResetTimeoutHandler(gpointer pTrigger)
    {
        return static_cast<OdeTrigger*>(pTrigger)->
            HandleResetTimeout();
    }

    int OdeTrigger::HandleResetTimeout()
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_resetTimerMutex);
        
        m_resetTimerId = 0;
        Reset();
        
        // One shot - return false.
        return false;
    }
    
    uint OdeTrigger::GetResetTimeout()
    {
        LOG_FUNC();
        
        return m_resetTimeout;
    }
        
    void OdeTrigger::SetResetTimeout(uint timeout)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_resetTimerMutex);
        
        // If the timer is currently running and the new 
        // timeout value is zero (disabled), then kill the timer.
        if (m_resetTimerId and !timeout)
        {
            g_source_remove(m_resetTimerId);
            m_resetTimerId = 0;
        }
        
        // Else, if the Timer is currently running and the new
        // timeout value is non-zero, stop and restart the timer.
        else if (m_resetTimerId and timeout)
        {
            g_source_remove(m_resetTimerId);
            m_resetTimerId = g_timeout_add(1000*m_resetTimeout, 
                TriggerResetTimeoutHandler, this);            
        }
        
        // Else, if the Trigger has reached its limit and the 
        // client is setting a Timeout value, start the timer.
        else if (m_limit and m_triggered >= m_limit and timeout)
        {
            m_resetTimerId = g_timeout_add(1000*m_resetTimeout, 
                TriggerResetTimeoutHandler, this);            
        } 
        
        m_resetTimeout = timeout;
    }
    
    bool OdeTrigger::IsResetTimerRunning()
    {
        LOG_FUNC();

        return m_resetTimerId;
    }
    
    bool OdeTrigger::AddLimitEventListener(
        dsl_ode_trigger_limit_event_listener_cb listener, void* clientData)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        if (m_limitEventListeners.find(listener) != 
            m_limitEventListeners.end())
        {   
            LOG_ERROR("Limit state change listener is not unique");
            return false;
        }
        m_limitEventListeners[listener] = clientData;

        return true;
    }
    
    bool OdeTrigger::RemoveLimitEventListener(
        dsl_ode_trigger_limit_event_listener_cb listener)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        if (m_limitEventListeners.find(listener) == 
            m_limitEventListeners.end())
        {   
            LOG_ERROR("Limit state change listener was not found");
            return false;
        }
        m_limitEventListeners.erase(listener);

        return true;
    }        
        
    uint OdeTrigger::GetClassId()
    {
        LOG_FUNC();
        
        return m_classId;
    }
    
    void OdeTrigger::SetClassId(uint classId)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_classId = classId;
    }

    uint OdeTrigger::GetLimit()
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        return m_limit;
    }
    
    void OdeTrigger::SetLimit(uint limit)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_limit = limit;
        
        // iterate through the map of limit-event-listeners calling each
        for(auto const& imap: m_limitEventListeners)
        {
            try
            {
                imap.first(DSL_ODE_TRIGGER_LIMIT_EVENT_LIMIT_CHANGED, 
                    m_limit, imap.second);
            }
            catch(...)
            {
                LOG_ERROR("Exception calling Client Limit-State-Change-Lister");
            }
        }
    }

    const char* OdeTrigger::GetSource()
    {
        LOG_FUNC();
        
        if (m_source.size())
        {
            return m_source.c_str();
        }
        return NULL;
    }
    
    void OdeTrigger::SetSource(const char* source)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_source.assign(source);
    }

    void OdeTrigger::_setSourceId(int id)
    {
        LOG_FUNC();
        
        m_sourceId = id;
    }
    
    const char* OdeTrigger::GetInfer()
    {
        LOG_FUNC();
        
        if (m_infer.size())
        {
            return m_infer.c_str();
        }
        return NULL;
    }
    
    void OdeTrigger::SetInfer(const char* infer)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_infer.assign(infer);
    }

    void OdeTrigger::_setInferId(int id)
    {
        LOG_FUNC();
        
        m_inferId = id;
    }
    
    float OdeTrigger::GetMinConfidence()
    {
        LOG_FUNC();
        
        return m_minConfidence;
    }
    
    void OdeTrigger::SetMinConfidence(float minConfidence)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_minConfidence = minConfidence;
    }
    
    void OdeTrigger::GetMinDimensions(float* minWidth, float* minHeight)
    {
        LOG_FUNC();
        
        *minWidth = m_minWidth;
        *minHeight = m_minHeight;
    }

    void OdeTrigger::SetMinDimensions(float minWidth, float minHeight)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_minWidth = minWidth;
        m_minHeight = minHeight;
    }
    
    void OdeTrigger::GetMaxDimensions(float* maxWidth, float* maxHeight)
    {
        LOG_FUNC();
        
        *maxWidth = m_maxWidth;
        *maxHeight = m_maxHeight;
    }

    void OdeTrigger::SetMaxDimensions(float maxWidth, float maxHeight)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_maxWidth = maxWidth;
        m_maxHeight = maxHeight;
    }
    
    bool OdeTrigger::GetInferDoneOnlySetting()
    {
        LOG_FUNC();
        
        return m_inferDoneOnly;
    }
    
    void OdeTrigger::SetInferDoneOnlySetting(bool inferDoneOnly)
    {
        LOG_FUNC();
        
        m_inferDoneOnly = inferDoneOnly;
    }
    
    void OdeTrigger::GetMinFrameCount(uint* minFrameCountN, uint* minFrameCountD)
    {
        LOG_FUNC();
        
        *minFrameCountN = m_minFrameCountN;
        *minFrameCountD = m_minFrameCountD;
    }

    void OdeTrigger::SetMinFrameCount(uint minFrameCountN, uint minFrameCountD)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_minFrameCountN = minFrameCountN;
        m_minFrameCountD = minFrameCountD;
    }

    uint OdeTrigger::GetInterval()
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        return m_interval;
    }
    
    void OdeTrigger::SetInterval(uint interval)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_interval = interval;
        m_intervalCounter = 0;
    }
    
    bool OdeTrigger::CheckForSourceId(int sourceId)
    {
        LOG_FUNC();

        // Filter on Source id if set
        if (m_source.size())
        {
            // a "one-time-get" of the source Id from the source name
            if (m_sourceId == -1)
            {
                
                Services::GetServices()->SourceIdGet(m_source.c_str(), &m_sourceId);
            }
            if (m_sourceId != sourceId)
            {
                return false;
            }
        }
        return true;
    }

    bool OdeTrigger::CheckForInferId(int inferId)
    {
        LOG_FUNC();

        // Filter on Source id if set
        if (m_infer.size())
        {
            // a "one-time-get" of the inference component Id from the name
            if (m_inferId == -1)
            {
                Services::GetServices()->InferIdGet(m_infer.c_str(), &m_inferId);
            }
            if (m_inferId != inferId)
            {
                return false;
            }
        }
        return true;
    }

    void OdeTrigger::PreProcessFrame(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta,
        NvDsFrameMeta* pFrameMeta)
    {
        // Reset the occurrences from the last frame, even if disabled  
        m_occurrences = 0;

        if (!m_enabled or !CheckForSourceId(pFrameMeta->source_id))
        {
            return;
        }

        // Call on each of the Trigger's Areas to (optionally) display their Rectangle
        for (const auto &imap: m_pOdeAreasIndexed)
        {
            DSL_ODE_AREA_PTR pOdeArea = std::dynamic_pointer_cast<OdeArea>(imap.second);
            
            pOdeArea->AddMeta(pDisplayMeta, pFrameMeta);
        }
        if (m_interval)
        {
            m_intervalCounter = (m_intervalCounter + 1) % m_interval; 
            if (m_intervalCounter != 0)
            {
                m_skipFrame = true;
                return;
            }
        }
        m_skipFrame = false;
    }

    bool OdeTrigger::CheckForMinCriteria(NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        // Note: function is called from the system (callback) context
        // Gaurd against property updates from the client API
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        // Filter on skip-frame interval
        if (m_skipFrame)
        {
            return false;
        }
        
        // Ensure enabled, and that the limit has not been exceeded
        if (m_limit and m_triggered >= m_limit) 
        {
            return false;
        }
        // Filter on unique source-id and unique-inference-component-id
        if (!CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForInferId(pObjectMeta->unique_component_id))
        {
            return false;
        }
        // Filter on Class id if set
        if ((m_classId != DSL_ODE_ANY_CLASS) and (m_classId != pObjectMeta->class_id))
        {
            return false;
        }
        // Ensure that the minimum confidence has been reached
        if (pObjectMeta->confidence > 0 and pObjectMeta->confidence < m_minConfidence)
        {
            return false;
        }
        // If defined, check for minimum dimensions
        if ((m_minWidth > 0 and pObjectMeta->rect_params.width < m_minWidth) or
            (m_minHeight > 0 and pObjectMeta->rect_params.height < m_minHeight))
        {
            return false;
        }
        // If defined, check for maximum dimensions
        if ((m_maxWidth > 0 and pObjectMeta->rect_params.width > m_maxWidth) or
            (m_maxHeight > 0 and pObjectMeta->rect_params.height > m_maxHeight))
        {
            return false;
        }
        // If define, check if Inference was done on the frame or not
        if (m_inferDoneOnly and !pFrameMeta->bInferDone)
        {
            return false;
        }
        return true;
    }

    bool OdeTrigger::CheckForWithin(NvDsObjectMeta* pObjectMeta)
    {
        // Note: function is called from the system (callback) context
        // Gaurd against property updates from the client API
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        // If areas are defined, check condition

        if (m_pOdeAreasIndexed.size())
        {
            for (const auto &imap: m_pOdeAreasIndexed)
            {
                DSL_ODE_AREA_PTR pOdeArea = std::dynamic_pointer_cast<OdeArea>(imap.second);
                if (pOdeArea->CheckForWithin(pObjectMeta->rect_params))
                {
                    return !pOdeArea->IsType(typeid(OdeExclusionArea));
                }
            }
            return false;
        }
        return true;
    }
    
    // *****************************************************************************
    AlwaysOdeTrigger::AlwaysOdeTrigger(const char* name, const char* source, uint when)
        : OdeTrigger(name, source, DSL_ODE_ANY_CLASS, 0)
        , m_when(when)
    {
        LOG_FUNC();
    }

    AlwaysOdeTrigger::~AlwaysOdeTrigger()
    {
        LOG_FUNC();
    }
    
    void AlwaysOdeTrigger::PreProcessFrame(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta,
        NvDsFrameMeta* pFrameMeta)
    {
        if (!m_enabled or !CheckForSourceId(pFrameMeta->source_id) or 
            m_when != DSL_ODE_PRE_OCCURRENCE_CHECK)
        {
            return;
        }
        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, NULL);
        }
    }

    uint AlwaysOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta,
        NvDsFrameMeta* pFrameMeta)
    {
        if (m_skipFrame or !m_enabled or !CheckForSourceId(pFrameMeta->source_id) or 
            m_when != DSL_ODE_POST_OCCURRENCE_CHECK)
        {
            return 0;
        }
        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, NULL);
        }
        return 1;
    }

    // *****************************************************************************

    OccurrenceOdeTrigger::OccurrenceOdeTrigger(const char* name, 
        const char* source, uint classId, uint limit)
        : OdeTrigger(name, source, classId, limit)
    {
        LOG_FUNC();
    }

    OccurrenceOdeTrigger::~OccurrenceOdeTrigger()
    {
        LOG_FUNC();
    }
    
    bool OccurrenceOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta,
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!m_enabled or !CheckForSourceId(pFrameMeta->source_id) 
            or !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }

        IncrementAndCheckTriggerCount();
        m_occurrences++;
        
        // update the total event count static variable
        s_eventCount++;

        // set the primary metric as the current occurrence for this frame
        pObjectMeta->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] = m_occurrences;

        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = std::dynamic_pointer_cast<OdeAction>(imap.second);
            try
            {
                pOdeAction->HandleOccurrence(shared_from_this(), pBuffer, 
                    pDisplayMeta, pFrameMeta, pObjectMeta);
            }
            catch(...)
            {
                LOG_ERROR("Trigger '" << GetName() << "' => Action '" 
                    << pOdeAction->GetName() << "' threw exception");
            }
        }
        return true;
    }

    // *****************************************************************************
    
    AbsenceOdeTrigger::AbsenceOdeTrigger(const char* name, 
        const char* source, uint classId, uint limit)
        : OdeTrigger(name, source, classId, limit)
    {
        LOG_FUNC();
    }

    AbsenceOdeTrigger::~AbsenceOdeTrigger()
    {
        LOG_FUNC();
    }
    
    bool AbsenceOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        // Important **** we need to check for Criteria even if the Absence Trigger is disabled. 
        // This is case another Trigger enables This trigger, and it checks for the number of 
        // occurrences in the PostProcessFrame() . If the m_occurrences is not updated the Trigger 
        // will report Absence incorrectly
        if (!CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }
        
        m_occurrences++;
        
        return true;
    }
    
    uint AbsenceOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        if (!m_enabled or (m_limit and m_triggered >= m_limit) or m_occurrences) 
        {
            return 0;
        }        
        
        // event has been triggered 
        IncrementAndCheckTriggerCount();

        // update the total event count static variable
        s_eventCount++;

        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, NULL);
        }
        return 1;
   }

    // *****************************************************************************

    AccumulationOdeTrigger::AccumulationOdeTrigger(const char* name, 
        const char* source, uint classId, uint limit)
        : OdeTrigger(name, source, classId, limit)
        , m_accumulativeOccurrences(0)
    {
        LOG_FUNC();
    }

    AccumulationOdeTrigger::~AccumulationOdeTrigger()
    {
        LOG_FUNC();
    }

    void AccumulationOdeTrigger::Reset()
    {
        LOG_FUNC();
        {
            LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
            
            m_accumulativeOccurrences = 0;
            m_instances.clear();
        }        
        // call the base class to complete the Reset
        OdeTrigger::Reset();
    }
    
    bool AccumulationOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta,
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!m_enabled or !CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }

        std::string sourceAndClassId = std::to_string(pFrameMeta->source_id) + "_" 
            + std::to_string(pObjectMeta->class_id);
            
        // If this is the first time seeing an object of "class_id" for "source_id".
        if (m_instances.find(sourceAndClassId) == m_instances.end())
        {
            // Initial the frame number for the new source
            m_instances[sourceAndClassId] = 0;
        }

        if (m_instances[sourceAndClassId] < pObjectMeta->object_id)
        {
            // Update the running instance
            m_instances[sourceAndClassId] = pObjectMeta->object_id;
            
            // Increment the Accumulative count. 
            m_accumulativeOccurrences++;
            
            // update for current frame
            m_occurrences = m_accumulativeOccurrences;
            
            return true;
        }
        // set to accumulative value always. Occurrences will be cleared in Pre-process frame.
        m_occurrences = m_accumulativeOccurrences;
        return false;
    }

    uint AccumulationOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        if (!m_enabled or m_skipFrame or (m_limit and m_triggered >= m_limit))
        {
            return 0;
        }
        // event has been triggered
        IncrementAndCheckTriggerCount();

         // update the total event count static variable
        s_eventCount++;

        // Add the  accumulates occurrences to the frame info
        pFrameMeta->misc_frame_info[DSL_FRAME_INFO_OCCURRENCES] = m_occurrences;

        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, NULL);
        }
        
        // return the running accumulative total
        return m_accumulativeOccurrences;
    }

    // *****************************************************************************
    
    TrackingOdeTrigger::TrackingOdeTrigger(const char* name, const char* source, 
        uint classId, uint limit, uint maxTracePoints)
        : OdeTrigger(name, source, classId, limit)
    {
        LOG_FUNC();
        
        m_pTrackedObjectsPerSource = std::shared_ptr<TrackedObjects>(
            new TrackedObjects(maxTracePoints));
    }

    TrackingOdeTrigger::~TrackingOdeTrigger()
    {
        LOG_FUNC();
    }

    void TrackingOdeTrigger::Reset()
    {
        LOG_FUNC();
        {
            LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
            
            m_pTrackedObjectsPerSource->Clear();
        }        
        // call the base class to complete the Reset
        OdeTrigger::Reset();
    }
    

    // *****************************************************************************
    
    CrossOdeTrigger::CrossOdeTrigger(const char* name, const char* source, 
        uint classId, uint limit, uint minTracePoints, uint maxTracePoints, 
        uint testMethod, DSL_RGBA_COLOR_PTR pColor)
        : TrackingOdeTrigger(name, source, classId, limit, maxTracePoints)
        , m_minTracePoints(minTracePoints)
        , m_maxTracePoints(maxTracePoints)
        , m_traceEnabled(false)
        , m_testMethod(testMethod)
        , m_pTraceColor(pColor)
        , m_traceLineWidth(0)
    {
        LOG_FUNC();
    }

    CrossOdeTrigger::~CrossOdeTrigger()
    {
        LOG_FUNC();
    }

    bool CrossOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!m_pOdeAreasIndexed.size())
        {
            LOG_ERROR("At least one OdeArea is required for CrossOdeTrigger '" 
                << GetName() << "'");
            return false;
        }

        // Note: we don't check for within area criteria until we have a trace.
        if (!CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta))
        {
            return false;
        }

        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        // if this is the first occurrence of any object for this source
        if (!m_pTrackedObjectsPerSource->IsTracked(pFrameMeta->source_id,
            pObjectMeta->object_id))
        {
            m_pTrackedObjectsPerSource->Track(pFrameMeta, pObjectMeta);
            return false;
        }

        std::shared_ptr<TrackedObject> pTrackedObject = 
            m_pTrackedObjectsPerSource->GetObject(pFrameMeta->source_id,
                pObjectMeta->object_id);
                
        pTrackedObject->Update(pFrameMeta->frame_num, &pObjectMeta->rect_params);
            
        for (const auto &imap: m_pOdeAreasIndexed)
        {
            DSL_ODE_AREA_PTR pOdeArea = std::dynamic_pointer_cast<OdeArea>(imap.second);
            
            // Get the trace vector for the testpoint defined for this Area
            std::shared_ptr<std::vector<dsl_coordinate>> pTrace = 
                pTrackedObject->GetTrace(pOdeArea->GetBboxTestPoint(), 
                    m_testMethod);

            if (m_traceEnabled)
            {
                // Create a RGBA multi-line from the trace vector and trace-view settings.
                DSL_RGBA_MULTI_LINE_PTR pMultiLine = DSL_RGBA_MULTI_LINE_NEW(
                    GetName().c_str(), pTrace->data(), pTrace->size(), 
                    m_traceLineWidth, m_pTraceColor);
                    
                // Add the multi-line's meta to the Frame's display-meta
                pMultiLine->AddMeta(pDisplayMeta, pFrameMeta);
            }
            
            if (pTrackedObject->Size() >= m_minTracePoints and 
                !pTrackedObject->GetTriggered() and pOdeArea->CheckForCross(pTrace))
            {
                // event has been triggered
                IncrementAndCheckTriggerCount();
                m_occurrences++;

                // update the total event count static variable
                s_eventCount++;
                    
                for (const auto &imap: m_pOdeActionsIndexed)
                {
                    DSL_ODE_ACTION_PTR pOdeAction = 
                        std::dynamic_pointer_cast<OdeAction>(imap.second);
                    pOdeAction->HandleOccurrence(shared_from_this(), 
                        pBuffer, pDisplayMeta, pFrameMeta, pObjectMeta);
                }
                // Once the object has crossed, mark as triggered.
                pTrackedObject->SetTriggered();
         
                return true;
            }
        }
        return false;
    }

    uint CrossOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        // Filter on skip-frame interval
        if (m_skipFrame)
        {
            return false;
        }
        if (m_pTrackedObjectsPerSource->IsEmpty())
        {
            return 0;
        }
        // purge all tracked objects, for all sources that are not in the current frame.
        m_pTrackedObjectsPerSource->Purge(pFrameMeta->frame_num);
        return m_occurrences;
    }
    
    void CrossOdeTrigger::GetTracePointSettings(uint* minTracePoints, 
        uint* maxTracePoints, uint* testMethod)
    {
        LOG_FUNC();

        *minTracePoints = m_minTracePoints;
        *maxTracePoints = m_maxTracePoints;
        *testMethod = m_testMethod;
    }
    
    void CrossOdeTrigger::SetTracePointSettings(uint minTracePoints,
        uint maxTracePoints, uint testMethod)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        m_minTracePoints = minTracePoints;
        m_maxTracePoints = maxTracePoints;
        m_testMethod = testMethod;
        m_pTrackedObjectsPerSource->SetMaxHistory(m_maxTracePoints);
    }
    
    void CrossOdeTrigger::GetTraceViewSettings(bool* enabled, 
        const char** color, uint* lineWidth)
    {
        LOG_FUNC();

        *enabled = m_traceEnabled;
        *color = m_pTraceColor->GetName().c_str();
        *lineWidth = m_traceLineWidth;
    }
    
    void CrossOdeTrigger::SetTraceViewSettings(bool enabled, DSL_RGBA_COLOR_PTR pColor, 
        uint lineWidth)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_traceEnabled = enabled;
        m_pTraceColor = pColor;
        m_traceLineWidth = lineWidth;
    }        
   
    // *****************************************************************************

    InstanceOdeTrigger::InstanceOdeTrigger(const char* name, 
        const char* source, uint classId, uint limit)
        : OdeTrigger(name, source, classId, limit)
    {
        LOG_FUNC();
    }

    InstanceOdeTrigger::~InstanceOdeTrigger()
    {
        LOG_FUNC();
    }

    void InstanceOdeTrigger::Reset()
    {
        LOG_FUNC();
        {
            LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
            
            m_instances.clear();
        }
        // call the base class to complete the Reset
        OdeTrigger::Reset();
    }
    
    bool InstanceOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta,
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!m_enabled or !CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }

        std::string sourceAndClassId = std::to_string(pFrameMeta->source_id) + "_" 
            + std::to_string(pObjectMeta->class_id);
            
        // If this is the first time seeing an object of "class_id" for "source_id".
        if (m_instances.find(sourceAndClassId) == m_instances.end())
        {
            // Initialize the frame number for the new source
            m_instances[sourceAndClassId] = 0;
        }
        if (m_instances[sourceAndClassId] < pObjectMeta->object_id)
        {
            // Update the running instance
            m_instances[sourceAndClassId] = pObjectMeta->object_id;
            
            IncrementAndCheckTriggerCount();
            m_occurrences++;
            
            // update the total event count static variable
            s_eventCount++;

            // set the primary metric to the new instance occurrence for this frame
            pObjectMeta->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] = m_occurrences;

            for (const auto &imap: m_pOdeActionsIndexed)
            {
                DSL_ODE_ACTION_PTR pOdeAction = 
                    std::dynamic_pointer_cast<OdeAction>(imap.second);
                try
                {
                    pOdeAction->HandleOccurrence(shared_from_this(), pBuffer, 
                        pDisplayMeta, pFrameMeta, pObjectMeta);
                }
                catch(...)
                {
                    LOG_ERROR("Trigger '" << GetName() << "' => Action '" 
                        << pOdeAction->GetName() << "' threw exception");
                }
            }
            return true;
        }
        return false;
    }

    // *****************************************************************************
    
    SummationOdeTrigger::SummationOdeTrigger(const char* name, 
        const char* source, uint classId, uint limit)
        : OdeTrigger(name, source, classId, limit)
    {
        LOG_FUNC();
    }

    SummationOdeTrigger::~SummationOdeTrigger()
    {
        LOG_FUNC();
    }
    
    bool SummationOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!m_enabled or !CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }
        
        m_occurrences++;
        
        return true;
    }

    uint SummationOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        if (!m_enabled or m_skipFrame or (m_limit and m_triggered >= m_limit))
        {
            return 0;
        }
        // event has been triggered
        IncrementAndCheckTriggerCount();

         // update the total event count static variable
        s_eventCount++;

        pFrameMeta->misc_frame_info[DSL_FRAME_INFO_OCCURRENCES] = m_occurrences;
        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = 
                std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, NULL);
        }
        return 1; // Summation ODE is triggered on every frame
   }

    // *****************************************************************************

    CustomOdeTrigger::CustomOdeTrigger(const char* name, const char* source, 
        uint classId, uint limit, dsl_ode_check_for_occurrence_cb clientChecker, 
        dsl_ode_post_process_frame_cb clientPostProcessor, void* clientData)
        : OdeTrigger(name, source, classId, limit)
        , m_clientChecker(clientChecker)
        , m_clientPostProcessor(clientPostProcessor)
        , m_clientData(clientData)
    {
        LOG_FUNC();
    }

    CustomOdeTrigger::~CustomOdeTrigger()
    {
        LOG_FUNC();
    }
    
    bool CustomOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        // conditional execution
        if (!m_enabled or !m_clientChecker or !CheckForSourceId(pFrameMeta->source_id) 
            or !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }
        try
        {
            if (!m_clientChecker(pBuffer, pFrameMeta, pObjectMeta, m_clientData))
            {
                return false;
            }
        }
        catch(...)
        {
            LOG_ERROR("Custon ODE Trigger '" << GetName() 
                << "' threw exception calling client callback");
            return false;
        }

        IncrementAndCheckTriggerCount();
        m_occurrences++;
        
        // update the total event count static variable
        s_eventCount++;

        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = 
                std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, pObjectMeta);
        }
        return true;
    }
    
    uint CustomOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        // conditional execution
        if (!m_enabled or m_clientPostProcessor == NULL)
        {
            return false;
        }
        try
        {
            if (!m_clientPostProcessor(pBuffer, pFrameMeta, m_clientData))
            {
                return 0;
            }
        }
        catch(...)
        {
            LOG_ERROR("Custon ODE Trigger '" << GetName() 
                << "' threw exception calling client callback");
            return false;
        }

        // event has been triggered
        IncrementAndCheckTriggerCount();

         // update the total event count static variable
        s_eventCount++;

        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = 
                std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, NULL);
        }
        return 1;
    }

    // *****************************************************************************
    
    PersistenceOdeTrigger::PersistenceOdeTrigger(const char* name, const char* source, 
        uint classId, uint limit, uint minimum, uint maximum)
        : TrackingOdeTrigger(name, source, classId, limit, 0)
        , m_minimumMs(minimum*1000.0)
        , m_maximumMs(maximum*1000.0)
    {
        LOG_FUNC();
    }

    PersistenceOdeTrigger::~PersistenceOdeTrigger()
    {
        LOG_FUNC();
    }

    void PersistenceOdeTrigger::GetRange(uint* minimum, uint* maximum)
    {
        LOG_FUNC();
        
        *minimum = m_minimumMs/1000;
        *maximum = m_maximumMs/1000;
    }

    void PersistenceOdeTrigger::SetRange(uint minimum, uint maximum)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_minimumMs = minimum*1000.0;
        m_maximumMs = maximum*1000.0;
    }
    
    bool PersistenceOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }

        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        // if this is the first occurrence of any object for this source
        if (!m_pTrackedObjectsPerSource->IsTracked(pFrameMeta->source_id,
            pObjectMeta->object_id))
        {
            m_pTrackedObjectsPerSource->Track(pFrameMeta, pObjectMeta);
        }
        else
        {
            std::shared_ptr<TrackedObject> pTrackedObject = 
                m_pTrackedObjectsPerSource->GetObject(pFrameMeta->source_id,
                    pObjectMeta->object_id);
                    
            pTrackedObject->Update(pFrameMeta->frame_num, &pObjectMeta->rect_params);

            double trackedTimeMs = pTrackedObject->GetDurationMs();
            
            LOG_DEBUG("Persistence for tracked object with id = " << pObjectMeta->object_id 
                << " for source = " << pFrameMeta->source_id << ", = " << trackedTimeMs << " ms");
            
            // if the object's tracked time is within range. 
            if (trackedTimeMs >= m_minimumMs and trackedTimeMs <= m_maximumMs)
            {
                // event has been triggered
                IncrementAndCheckTriggerCount();
                m_occurrences++;

                // update the total event count static variable
                s_eventCount++;
    
                // add the persistence value to the array of misc_obj_info
                // as both the Primary and Persistence specific indecies.
                pObjectMeta->misc_obj_info[DSL_OBJECT_INFO_PERSISTENCE] = 
                pObjectMeta->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] = 
                    (uint64_t)(trackedTimeMs/1000);
                    
                for (const auto &imap: m_pOdeActionsIndexed)
                {
                    DSL_ODE_ACTION_PTR pOdeAction = 
                        std::dynamic_pointer_cast<OdeAction>(imap.second);
                    pOdeAction->HandleOccurrence(shared_from_this(), 
                        pBuffer, pDisplayMeta, pFrameMeta, pObjectMeta);
                }
            }
        }
        return true;
    }

    uint PersistenceOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        if (m_pTrackedObjectsPerSource->IsEmpty())
        {
            return 0;
        }
        // purge all tracked objects, for all sources that are not in the current frame.
        m_pTrackedObjectsPerSource->Purge(pFrameMeta->frame_num);
        return m_occurrences;
    }

    // *****************************************************************************
    
    CountOdeTrigger::CountOdeTrigger(const char* name, const char* source,
        uint classId, uint limit, uint minimum, uint maximum)
        : OdeTrigger(name, source, classId, limit)
        , m_minimum(minimum)
        , m_maximum(maximum)
    {
        LOG_FUNC();
    }

    CountOdeTrigger::~CountOdeTrigger()
    {
        LOG_FUNC();
    }

    void CountOdeTrigger::GetRange(uint* minimum, uint* maximum)
    {
        LOG_FUNC();
        
        *minimum = m_minimum;
        *maximum = m_maximum;
    }

    void CountOdeTrigger::SetRange(uint minimum, uint maximum)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_minimum = minimum;
        m_maximum = maximum;
    }
    
    bool CountOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }
        
        m_occurrences++;
        
        return true;
    }

    uint CountOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        if (!m_enabled or (m_occurrences < m_minimum) or (m_occurrences > m_maximum))
        {
            return 0;
        }
        // event has been triggered
        IncrementAndCheckTriggerCount();

         // update the total event count static variable
        s_eventCount++;

        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = 
                std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, NULL);
        }
        return m_occurrences;
   }

    // *****************************************************************************
    
    SmallestOdeTrigger::SmallestOdeTrigger(const char* name, 
        const char* source, uint classId, uint limit)
        : OdeTrigger(name, source, classId, limit)
    {
        LOG_FUNC();
    }

    SmallestOdeTrigger::~SmallestOdeTrigger()
    {
        LOG_FUNC();
    }
    
    bool SmallestOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }
        
        m_occurrenceMetaList.push_back(pObjectMeta);
        
        return true;
    }

    uint SmallestOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        m_occurrences = 0;
        
        // need at least one object for a Minimum event
        if (m_enabled and m_occurrenceMetaList.size())
        {
            // One occurrence to return and increment the accumulative Trigger count
            m_occurrences = 1;
            IncrementAndCheckTriggerCount();
            // update the total event count static variable
            s_eventCount++;

            uint smallestArea = UINT32_MAX;
            NvDsObjectMeta* smallestObject(NULL);
            
            // iterate through the list of object occurrences that passed all min criteria
            for (const auto &ivec: m_occurrenceMetaList) 
            {
                uint rectArea = ivec->rect_params.width * ivec->rect_params.height;
                if (rectArea < smallestArea) 
                { 
                    smallestArea = rectArea;
                    smallestObject = ivec;    
                }
            }
            // set the primary metric as the smallest bounding box by area
            smallestObject->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] = smallestArea;
            for (const auto &imap: m_pOdeActionsIndexed)
            {
                DSL_ODE_ACTION_PTR pOdeAction = 
                    std::dynamic_pointer_cast<OdeAction>(imap.second);
                
                pOdeAction->HandleOccurrence(shared_from_this(), 
                    pBuffer, pDisplayMeta, pFrameMeta, smallestObject);
            }
        }   

        // reset for next frame
        m_occurrenceMetaList.clear();
        return m_occurrences;
   }

    // *****************************************************************************
    
    LargestOdeTrigger::LargestOdeTrigger(const char* name, 
        const char* source, uint classId, uint limit)
        : OdeTrigger(name, source, classId, limit)
    {
        LOG_FUNC();
    }

    LargestOdeTrigger::~LargestOdeTrigger()
    {
        LOG_FUNC();
    }
    
    bool LargestOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }
        
        m_occurrenceMetaList.push_back(pObjectMeta);
        
        return true;
    }

    uint LargestOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        m_occurrences = 0;
        
        // need at least one object for a Minimum event
        if (m_enabled and m_occurrenceMetaList.size())
        {
            // Once occurrence to return and increment the accumulative Trigger count
            m_occurrences = 1;
            IncrementAndCheckTriggerCount();
            // update the total event count static variable
            s_eventCount++;

            uint largestArea = 0;
            NvDsObjectMeta* largestObject(NULL);
            
            // iterate through the list of object occurrences that passed all min criteria
            for (const auto &ivec: m_occurrenceMetaList) 
            {
                uint rectArea = ivec->rect_params.width * ivec->rect_params.height;
                if (rectArea > largestArea) 
                { 
                    largestArea = rectArea;
                    largestObject = ivec;    
                }
            }
            // set the primary metric as the larget area
            largestObject->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] = largestArea;
            
            for (const auto &imap: m_pOdeActionsIndexed)
            {
                DSL_ODE_ACTION_PTR pOdeAction = 
                    std::dynamic_pointer_cast<OdeAction>(imap.second);
                
                pOdeAction->HandleOccurrence(shared_from_this(), 
                    pBuffer, pDisplayMeta, pFrameMeta, largestObject);
            }
        }   

        // reset for next frame
        m_occurrenceMetaList.clear();
        return m_occurrences;
    }

    // *****************************************************************************
    
    LatestOdeTrigger::LatestOdeTrigger(const char* name, const char* source, 
        uint classId, uint limit)
        : TrackingOdeTrigger(name, source, classId, limit, 0)
        , m_pLatestObjectMeta(NULL)
        , m_latestTrackedTimeMs(0)
    {
        LOG_FUNC();
    }

    LatestOdeTrigger::~LatestOdeTrigger()
    {
        LOG_FUNC();
    }

    bool LatestOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }

        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        // if this is the first occurrence of any object for this source
        if (!m_pTrackedObjectsPerSource->IsTracked(pFrameMeta->source_id,
            pObjectMeta->object_id))
        {
            m_pTrackedObjectsPerSource->Track(pFrameMeta, pObjectMeta);
        }
        else
        {
            std::shared_ptr<TrackedObject> pTrackedObject = 
                m_pTrackedObjectsPerSource->GetObject(pFrameMeta->source_id,
                    pObjectMeta->object_id);
                    
            pTrackedObject->Update(pFrameMeta->frame_num, &pObjectMeta->rect_params);

            double trackedTimeMs = pTrackedObject->GetDurationMs();
            
            if ((m_pLatestObjectMeta == NULL) or (trackedTimeMs < m_latestTrackedTimeMs))
            {
                m_pLatestObjectMeta = pObjectMeta;
                m_latestTrackedTimeMs = trackedTimeMs;
            }
        }
        return true;
    }
    
    uint LatestOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        if (m_pTrackedObjectsPerSource->IsEmpty())
        {
            return 0;
        }
        
        // If we a Newest Object ODE 
        if (m_pLatestObjectMeta != NULL)
        {
            // event has been triggered
            IncrementAndCheckTriggerCount();
            m_occurrences++;

            // update the total event count static variable
            s_eventCount++;
            
            // add the persistence value to the array of misc_obj_info
            // as both the Primary and Persistence specific indecies.
            m_pLatestObjectMeta->misc_obj_info[DSL_OBJECT_INFO_PERSISTENCE] = 
            m_pLatestObjectMeta->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] = 
                (uint64_t)(m_latestTrackedTimeMs/1000);

            for (const auto &imap: m_pOdeActionsIndexed)
            {
                DSL_ODE_ACTION_PTR pOdeAction = 
                    std::dynamic_pointer_cast<OdeAction>(imap.second);
                pOdeAction->HandleOccurrence(shared_from_this(), 
                    pBuffer, pDisplayMeta, pFrameMeta, m_pLatestObjectMeta);
            }
        
            // clear the Newest Object data for the next frame 
            m_pLatestObjectMeta = NULL;
            m_latestTrackedTimeMs = 0;
        }
        
        // purge all tracked objects, for all sources that are not in the current frame.
        m_pTrackedObjectsPerSource->Purge(pFrameMeta->frame_num);
        
        return (m_occurrences > 0);
    }

    // *****************************************************************************
    
    EarliestOdeTrigger::EarliestOdeTrigger(const char* name, const char* source, 
        uint classId, uint limit)
        : TrackingOdeTrigger(name, source, classId, limit, 0)
        , m_pEarliestObjectMeta(NULL)
        , m_earliestTrackedTimeMs(0)
    {
        LOG_FUNC();
    }

    EarliestOdeTrigger::~EarliestOdeTrigger()
    {
        LOG_FUNC();
    }

    bool EarliestOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }

        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        // if this is the first occurrence of any object for this source
        if (!m_pTrackedObjectsPerSource->IsTracked(pFrameMeta->source_id,
            pObjectMeta->object_id)) 
        {
            m_pTrackedObjectsPerSource->Track(pFrameMeta, pObjectMeta);
        }
        else
        {
            std::shared_ptr<TrackedObject> pTrackedObject = 
                m_pTrackedObjectsPerSource->GetObject(pFrameMeta->source_id,
                    pObjectMeta->object_id);
                    
            pTrackedObject->Update(pFrameMeta->frame_num, &pObjectMeta->rect_params);

            double trackedTimeMs = pTrackedObject->GetDurationMs();
                
            if ((m_pEarliestObjectMeta == NULL) or (trackedTimeMs > m_earliestTrackedTimeMs))
            {
                m_pEarliestObjectMeta = pObjectMeta;
                m_earliestTrackedTimeMs = trackedTimeMs;
                
            }
        }
        return true;
    }
    
    uint EarliestOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        if (m_pTrackedObjectsPerSource->IsEmpty())
        {
            return 0;
        }
        
        if (m_pEarliestObjectMeta != NULL)
        {
            // event has been triggered
            IncrementAndCheckTriggerCount();
            m_occurrences++;

            // update the total event count static variable
            s_eventCount++;

            // add the persistence value to the array of misc_obj_info
            // as both the Primary and Persistence specific indecies.
            m_pEarliestObjectMeta->misc_obj_info[DSL_OBJECT_INFO_PERSISTENCE] = 
            m_pEarliestObjectMeta->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] = 
                (uint64_t)(m_earliestTrackedTimeMs/1000);

            for (const auto &imap: m_pOdeActionsIndexed)
            {
                DSL_ODE_ACTION_PTR pOdeAction = 
                    std::dynamic_pointer_cast<OdeAction>(imap.second);
                pOdeAction->HandleOccurrence(shared_from_this(), 
                    pBuffer, pDisplayMeta, pFrameMeta, m_pEarliestObjectMeta);
            }
        
            // clear the Earliest Object data for the next frame 
            m_pEarliestObjectMeta = NULL;
            m_earliestTrackedTimeMs = 0;
        }
        
        // purge all tracked objects, for all sources that are not in the current frame.
        m_pTrackedObjectsPerSource->Purge(pFrameMeta->frame_num);
        return (m_occurrences > 0);
    }

    // *****************************************************************************
    
    NewLowOdeTrigger::NewLowOdeTrigger(const char* name, 
        const char* source, uint classId, uint limit, uint preset)
        : OdeTrigger(name, source, classId, limit)
        , m_preset(preset)
        , m_currentLow(preset)
        
    {
        LOG_FUNC();
    }

    NewLowOdeTrigger::~NewLowOdeTrigger()
    {
        LOG_FUNC();
    }

    void NewLowOdeTrigger::Reset()
    {
        LOG_FUNC();
        {
            LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
            
            m_currentLow = m_preset;
        }        
        // call the base class to complete the Reset
        OdeTrigger::Reset();
    }
    
    bool NewLowOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!m_enabled or !CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }
        
        m_occurrences++;
        
        return true;
    }

    uint NewLowOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        if (!m_enabled or m_occurrences >= m_currentLow)
        {
            return 0;
        }
        // new low
        m_currentLow = m_occurrences;
        
        // event has been triggered
        IncrementAndCheckTriggerCount();

         // update the total event count static variable
        s_eventCount++;

        // Add the New High occurrences to the frame info
        pFrameMeta->misc_frame_info[DSL_FRAME_INFO_OCCURRENCES] = m_occurrences;

        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = 
                std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, NULL);
        }
        return 1; // At most once per frame
   }

    // *****************************************************************************
    
    NewHighOdeTrigger::NewHighOdeTrigger(const char* name, 
        const char* source, uint classId, uint limit, uint preset)
        : OdeTrigger(name, source, classId, limit)
        , m_preset(preset)
        , m_currentHigh(preset)
        
    {
        LOG_FUNC();
    }

    NewHighOdeTrigger::~NewHighOdeTrigger()
    {
        LOG_FUNC();
    }

    void NewHighOdeTrigger::Reset()
    {
        LOG_FUNC();
        {
            LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
            
            m_currentHigh = m_preset;
        }        
        // call the base class to complete the Reset
        OdeTrigger::Reset();
    }
    
    bool NewHighOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!m_enabled or !CheckForSourceId(pFrameMeta->source_id) or 
            !CheckForMinCriteria(pFrameMeta, pObjectMeta) or !CheckForWithin(pObjectMeta))
        {
            return false;
        }
        
        m_occurrences++;
        
        return true;
    }

    uint NewHighOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        if (!m_enabled or m_occurrences <= m_currentHigh)
        {
            return 0;
        }
        // new high
        m_currentHigh = m_occurrences;
        
        // event has been triggered
        IncrementAndCheckTriggerCount();

         // update the total event count static variable
        s_eventCount++;

        // Add the New High occurrences to the frame info
        pFrameMeta->misc_frame_info[DSL_FRAME_INFO_OCCURRENCES] = m_occurrences;

        for (const auto &imap: m_pOdeActionsIndexed)
        {
            DSL_ODE_ACTION_PTR pOdeAction = 
                std::dynamic_pointer_cast<OdeAction>(imap.second);
            pOdeAction->HandleOccurrence(shared_from_this(), 
                pBuffer, pDisplayMeta, pFrameMeta, NULL);
        }
        return 1; // At most once per frame
   }
   
    // *****************************************************************************
    
    ABOdeTrigger::ABOdeTrigger(const char* name, 
        const char* source, uint classIdA, uint classIdB, uint limit)
        : OdeTrigger(name, source, classIdA, limit)
        , m_classIdA(classIdA)
        , m_classIdB(classIdB)
    {
        LOG_FUNC();
        
        m_classIdAOnly = (m_classIdA == m_classIdB);
    }

    ABOdeTrigger::~ABOdeTrigger()
    {
        LOG_FUNC();
    }

    void ABOdeTrigger::GetClassIdAB(uint* classIdA, uint* classIdB)
    {
        LOG_FUNC();
        
        *classIdA = m_classIdA;
        *classIdB = m_classIdB;
    }
    
    void ABOdeTrigger::SetClassIdAB(uint classIdA, uint classIdB)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_classIdA = classIdA;
        m_classIdB = classIdB;
        m_classIdAOnly = (m_classIdA == m_classIdB);
    }
    
    bool ABOdeTrigger::CheckForOccurrence(GstBuffer* pBuffer, NvDsDisplayMeta* pDisplayMeta, 
        NvDsFrameMeta* pFrameMeta, NvDsObjectMeta* pObjectMeta)
    {
        if (!m_enabled or !CheckForSourceId(pFrameMeta->source_id))
        {
            return false;
        }
        
        bool occurrenceAdded(false);
        
        m_classId = m_classIdA;
        if (CheckForMinCriteria(pFrameMeta, pObjectMeta) and CheckForWithin(pObjectMeta))
        {
            m_occurrenceMetaListA.push_back(pObjectMeta);
            occurrenceAdded = true;
        }
        else if (!m_classIdAOnly)
        {
            m_classId = m_classIdB;
            if (CheckForMinCriteria(pFrameMeta, pObjectMeta) and CheckForWithin(pObjectMeta))
            {
                m_occurrenceMetaListB.push_back(pObjectMeta);
                occurrenceAdded = true;
            }
        }
        
        return occurrenceAdded;
    }

    uint ABOdeTrigger::PostProcessFrame(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        if (m_classIdAOnly)
        {
            return PostProcessFrameA(pBuffer, pDisplayMeta, pFrameMeta);
        }
        return  PostProcessFrameAB(pBuffer, pDisplayMeta, pFrameMeta);
    }

    // *****************************************************************************
    
    DistanceOdeTrigger::DistanceOdeTrigger(const char* name, const char* source, 
        uint classIdA, uint classIdB, uint limit, uint minimum, uint maximum, 
        uint testPoint, uint testMethod)
        : ABOdeTrigger(name, source, classIdA, classIdB, limit)
        , m_minimum(minimum)
        , m_maximum(maximum)
        , m_testPoint(testPoint)
        , m_testMethod(testMethod)
    {
        LOG_FUNC();
    }

    DistanceOdeTrigger::~DistanceOdeTrigger()
    {
        LOG_FUNC();
    }

    void DistanceOdeTrigger::GetRange(uint* minimum, uint* maximum)
    {
        LOG_FUNC();
        
        *minimum = m_minimum;
        *maximum = m_maximum;
    }

    void DistanceOdeTrigger::SetRange(uint minimum, uint maximum)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_minimum = minimum;
        m_maximum = maximum;
    }

    void DistanceOdeTrigger::GetTestParams(uint* testPoint, uint* testMethod)
    {
        LOG_FUNC();

        *testPoint = m_testPoint;
        *testMethod = m_testMethod;
    }

    void DistanceOdeTrigger::SetTestParams(uint testPoint, uint testMethod)
    {
        LOG_FUNC();
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);
        
        m_testPoint = testPoint;
        m_testMethod = testMethod;
    }
    
    
    uint DistanceOdeTrigger::PostProcessFrameA(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        m_occurrences = 0;
        
        // need at least two objects for intersection to occur
        if (m_enabled and m_occurrenceMetaListA.size() > 1)
        {
            // iterate through the list of object occurrences that passed all min criteria
            for (uint i = 0; i < m_occurrenceMetaListA.size()-1 ; i++) 
            {
                for (uint j = i+1; j < m_occurrenceMetaListA.size() ; j++) 
                {
                    if (CheckDistance(m_occurrenceMetaListA[i], m_occurrenceMetaListA[j]))
                    {
                        // event has been triggered
                        m_occurrences++;
                        IncrementAndCheckTriggerCount();
                        
                         // update the total event count static variable
                        s_eventCount++;

                        // set the primary metric as the current occurrence for this frame
                        m_occurrenceMetaListA[i]->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] 
                            = m_occurrences;
                        m_occurrenceMetaListA[j]->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] 
                            = m_occurrences;

                        for (const auto &imap: m_pOdeActionsIndexed)
                        {
                            DSL_ODE_ACTION_PTR pOdeAction = 
                                std::dynamic_pointer_cast<OdeAction>(imap.second);
                            
                            // Invoke each action twice, once for each object in the tested pair
                            pOdeAction->HandleOccurrence(shared_from_this(), 
                                pBuffer, pDisplayMeta, pFrameMeta, m_occurrenceMetaListA[i]);
                            pOdeAction->HandleOccurrence(shared_from_this(), 
                                pBuffer, pDisplayMeta, pFrameMeta, m_occurrenceMetaListA[j]);
                        }
                        if (m_limit and m_triggered >= m_limit)
                        {
                            m_occurrenceMetaListA.clear();
                            return m_occurrences;
                        }
                    }
                }
            }
        }   

        // reset for next frame
        m_occurrenceMetaListA.clear();
        return m_occurrences;
    }
   
    uint DistanceOdeTrigger::PostProcessFrameAB(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        m_occurrences = 0;
        
        // need at least one object from each of the two Classes 
        if (m_enabled and m_occurrenceMetaListA.size() and m_occurrenceMetaListB.size())
        {
            // iterate through the list of object occurrences that passed all min criteria
            for (const auto &iterA: m_occurrenceMetaListA) 
            {
                for (const auto &iterB: m_occurrenceMetaListB) 
                {
                    // ensure we are not testing the same object which can be in both vectors
                    // if Class Id A and B are specified to be the same.
                    if (iterA != iterB)
                    {
                        if (CheckDistance(iterA, iterB))
                        {
                            // event has been triggered
                            m_occurrences++;
                            IncrementAndCheckTriggerCount();
                            
                             // update the total event count static variable
                            s_eventCount++;

                            // set the primary metric as the current occurrence for this frame
                            iterA->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] 
                                = m_occurrences;
                            iterB->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] 
                                = m_occurrences;

                            for (const auto &imap: m_pOdeActionsIndexed)
                            {
                                DSL_ODE_ACTION_PTR pOdeAction = 
                                    std::dynamic_pointer_cast<OdeAction>(imap.second);
                                
                                // Invoke each action twice, once for each object in the tested pair
                                pOdeAction->HandleOccurrence(shared_from_this(), 
                                    pBuffer, pDisplayMeta, pFrameMeta, iterA);
                                pOdeAction->HandleOccurrence(shared_from_this(), 
                                    pBuffer, pDisplayMeta, pFrameMeta, iterB);
                            }
                            if (m_limit and m_triggered >= m_limit)
                            {
                                m_occurrenceMetaListA.clear();
                                m_occurrenceMetaListB.clear();
                                return m_occurrences;
                            }
                        }
                    }
                }
            }
        }   

        // reset for next frame
        m_occurrenceMetaListA.clear();
        m_occurrenceMetaListB.clear();
        return m_occurrences;
    }

    bool DistanceOdeTrigger::CheckDistance(NvDsObjectMeta* pObjectMetaA, NvDsObjectMeta* pObjectMetaB)
    {
        LOCK_MUTEX_FOR_CURRENT_SCOPE(&m_propertyMutex);

        uint distance(0);
        if (m_testPoint == DSL_BBOX_POINT_ANY)
        {
            GeosRectangle rectA(pObjectMetaA->rect_params);
            GeosRectangle rectB(pObjectMetaB->rect_params);
            distance = rectA.Distance(rectB);
        }
        else{
            uint xa(0), ya(0), xb(0), yb(0);
            switch (m_testPoint)
            {
            case DSL_BBOX_POINT_CENTER :
                xa = round(pObjectMetaA->rect_params.left + pObjectMetaA->rect_params.width/2);
                ya = round(pObjectMetaA->rect_params.top + pObjectMetaA->rect_params.height/2);
                xb = round(pObjectMetaB->rect_params.left + pObjectMetaB->rect_params.width/2);
                yb = round(pObjectMetaB->rect_params.top + pObjectMetaB->rect_params.height/2);
                break;
            case DSL_BBOX_POINT_NORTH_WEST :
                xa = round(pObjectMetaA->rect_params.left);
                ya = round(pObjectMetaA->rect_params.top);
                xb = round(pObjectMetaB->rect_params.left);
                yb = round(pObjectMetaB->rect_params.top);
                break;
            case DSL_BBOX_POINT_NORTH :
                xa = round(pObjectMetaA->rect_params.left + pObjectMetaA->rect_params.width/2);
                ya = round(pObjectMetaA->rect_params.top);
                xb = round(pObjectMetaB->rect_params.left + pObjectMetaB->rect_params.width/2);
                yb = round(pObjectMetaB->rect_params.top);
                break;
            case DSL_BBOX_POINT_NORTH_EAST :
                xa = round(pObjectMetaA->rect_params.left + pObjectMetaA->rect_params.width);
                ya = round(pObjectMetaA->rect_params.top);
                xb = round(pObjectMetaB->rect_params.left + pObjectMetaB->rect_params.width);
                yb = round(pObjectMetaB->rect_params.top);
                break;
            case DSL_BBOX_POINT_EAST :
                xa = round(pObjectMetaA->rect_params.left + pObjectMetaA->rect_params.width);
                ya = round(pObjectMetaA->rect_params.top + pObjectMetaA->rect_params.height/2);
                xb = round(pObjectMetaB->rect_params.left + pObjectMetaB->rect_params.width);
                yb = round(pObjectMetaB->rect_params.top + pObjectMetaB->rect_params.height/2);
                break;
            case DSL_BBOX_POINT_SOUTH_EAST :
                xa = round(pObjectMetaA->rect_params.left + pObjectMetaA->rect_params.width);
                ya = round(pObjectMetaA->rect_params.top + pObjectMetaA->rect_params.height);
                xb = round(pObjectMetaB->rect_params.left + pObjectMetaB->rect_params.width);
                yb = round(pObjectMetaB->rect_params.top + pObjectMetaB->rect_params.height);
                break;
            case DSL_BBOX_POINT_SOUTH :
                xa = round(pObjectMetaA->rect_params.left + pObjectMetaA->rect_params.width/2);
                ya = round(pObjectMetaA->rect_params.top + pObjectMetaA->rect_params.height);
                xb = round(pObjectMetaB->rect_params.left + pObjectMetaB->rect_params.width/2);
                yb = round(pObjectMetaB->rect_params.top + pObjectMetaB->rect_params.height);
                break;
            case DSL_BBOX_POINT_SOUTH_WEST :
                xa = round(pObjectMetaA->rect_params.left);
                ya = round(pObjectMetaA->rect_params.top + pObjectMetaA->rect_params.height);
                xb = round(pObjectMetaB->rect_params.left);
                yb = round(pObjectMetaB->rect_params.top + pObjectMetaB->rect_params.height);
                break;
            case DSL_BBOX_POINT_WEST :
                xa = round(pObjectMetaA->rect_params.left);
                ya = round(pObjectMetaA->rect_params.top + pObjectMetaA->rect_params.height/2);
                xb = round(pObjectMetaB->rect_params.left);
                yb = round(pObjectMetaB->rect_params.top + pObjectMetaB->rect_params.height/2);
                break;
            default:
                LOG_ERROR("Invalid DSL_BBOX_POINT = '" << m_testPoint 
                    << "' for DistanceOdeTrigger Trigger '" << GetName() << "'");
                throw;
            }

            GeosPoint pointA(xa, ya);
            GeosPoint pointB(xb, yb);
            distance = pointA.Distance(pointB);
        }
        
        uint minimum(0), maximum(0);
        switch (m_testMethod)
        {
        case DSL_DISTANCE_METHOD_FIXED_PIXELS :
            minimum = m_minimum;
            maximum = m_maximum;
            break;
        case DSL_DISTANCE_METHOD_PERCENT_WIDTH_A :
            minimum = uint((m_minimum*pObjectMetaA->rect_params.width)/100);
            maximum = uint((m_maximum*pObjectMetaA->rect_params.width)/100);
            break;
        case DSL_DISTANCE_METHOD_PERCENT_WIDTH_B :
            minimum = uint((m_minimum*pObjectMetaB->rect_params.width)/100);
            maximum = uint((m_maximum*pObjectMetaB->rect_params.width)/100);
            break;
        case DSL_DISTANCE_METHOD_PERCENT_HEIGHT_A :
            minimum = uint((m_minimum*pObjectMetaA->rect_params.height)/100);
            maximum = uint((m_maximum*pObjectMetaA->rect_params.height)/100);
            break;
        case DSL_DISTANCE_METHOD_PERCENT_HEIGHT_B :
            minimum = uint((m_minimum*pObjectMetaB->rect_params.height)/100);
            maximum = uint((m_maximum*pObjectMetaB->rect_params.height)/100);
            break;
        }    
        return (minimum > distance or maximum < distance);
    }

    // *****************************************************************************
    
    IntersectionOdeTrigger::IntersectionOdeTrigger(const char* name, 
        const char* source, uint classIdA, uint classIdB, uint limit)
        : ABOdeTrigger(name, source, classIdA, classIdB, limit)
    {
        LOG_FUNC();
    }

    IntersectionOdeTrigger::~IntersectionOdeTrigger()
    {
        LOG_FUNC();
    }

    
    uint IntersectionOdeTrigger::PostProcessFrameA(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        m_occurrences = 0;
        
        // need at least two objects for intersection to occur
        if (m_enabled and m_occurrenceMetaListA.size() > 1)
        {
            // iterate through the list of object occurrences that passed all min criteria
            for (uint i = 0; i < m_occurrenceMetaListA.size()-1 ; i++) 
            {
                for (uint j = i+1; j < m_occurrenceMetaListA.size() ; j++) 
                {
                    // check each in turn for any frame overlap
                    GeosRectangle rectA(m_occurrenceMetaListA[i]->rect_params);
                    GeosRectangle rectB(m_occurrenceMetaListA[j]->rect_params);
                    if (rectA.Overlaps(rectB))
                    {
                        // event has been triggered
                        m_occurrences++;
                        IncrementAndCheckTriggerCount();
                        
                         // update the total event count static variable
                        s_eventCount++;

                        // set the primary metric as the current occurrence for this frame
                        m_occurrenceMetaListA[i]->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] 
                            = m_occurrences;
                        m_occurrenceMetaListA[j]->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] 
                            = m_occurrences;

                        for (const auto &imap: m_pOdeActionsIndexed)
                        {
                            DSL_ODE_ACTION_PTR pOdeAction = 
                                std::dynamic_pointer_cast<OdeAction>(imap.second);
                            
                            // Invoke each action twice, once for each object in the tested pair
                            pOdeAction->HandleOccurrence(shared_from_this(), 
                                pBuffer, pDisplayMeta, pFrameMeta, m_occurrenceMetaListA[i]);
                            pOdeAction->HandleOccurrence(shared_from_this(), 
                                pBuffer, pDisplayMeta, pFrameMeta, m_occurrenceMetaListA[j]);
                        }
                        if (m_limit and m_triggered >= m_limit)
                        {
                            m_occurrenceMetaListA.clear();
                            return m_occurrences;
                        }
                    }
                }
            }
        }   

        // reset for next frame
        m_occurrenceMetaListA.clear();
        return m_occurrences;
   }

    uint IntersectionOdeTrigger::PostProcessFrameAB(GstBuffer* pBuffer, 
        NvDsDisplayMeta* pDisplayMeta,  NvDsFrameMeta* pFrameMeta)
    {
        m_occurrences = 0;
        
        // need at least one object from each of the two Classes 
        if (m_enabled and m_occurrenceMetaListA.size() and m_occurrenceMetaListB.size())
        {
            // iterate through the list of object occurrences that passed all min criteria
            for (const auto &iterA: m_occurrenceMetaListA) 
            {
                for (const auto &iterB: m_occurrenceMetaListB) 
                {
                    // ensure we are not testing the same object which can be in both vectors
                    // if Class Id A and B are specified to be the same.
                    if (iterA != iterB)
                    {
                        // check each in turn for any frame overlap
                        GeosRectangle rectA(iterA->rect_params);
                        GeosRectangle rectB(iterB->rect_params);
                        if (rectA.Overlaps(rectB))
                        {
                            // event has been triggered
                            m_occurrences++;
                            IncrementAndCheckTriggerCount();
                            
                             // update the total event count static variable
                            s_eventCount++;

                            // set the primary metric as the current occurrence 
                            // for this frame
                            iterA->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] 
                                = m_occurrences;
                            iterB->misc_obj_info[DSL_OBJECT_INFO_PRIMARY_METRIC] 
                                = m_occurrences;
                            
                            for (const auto &imap: m_pOdeActionsIndexed)
                            {
                                DSL_ODE_ACTION_PTR pOdeAction = 
                                    std::dynamic_pointer_cast<OdeAction>(imap.second);
                                
                                // Invoke each action twice, once for each object 
                                // in the tested pair
                                pOdeAction->HandleOccurrence(shared_from_this(), 
                                    pBuffer, pDisplayMeta, pFrameMeta, iterA);
                                pOdeAction->HandleOccurrence(shared_from_this(), 
                                    pBuffer, pDisplayMeta, pFrameMeta, iterB);
                            }
                            if (m_limit and m_triggered >= m_limit)
                            {
                                m_occurrenceMetaListA.clear();
                                m_occurrenceMetaListB.clear();
                                return m_occurrences;
                            }
                        }
                    }
                }
            }
        }   

        // reset for next frame
        m_occurrenceMetaListA.clear();
        m_occurrenceMetaListB.clear();
        return m_occurrences;
    }
}
//
// Copyright (c) Microsoft. All rights reserved.
// Licensed under the MIT license. See LICENSE.md file in the project root for full license information.
//
#pragma once

#include "Basics.h"
#include "Matrix.h"
#include "TensorView.h"
#include "ScriptableObjects.h"
#include "Sequences.h"
#include "TensorShape.h"
#include "MatrixPool.h"

#include <unordered_set>
#include <map>
#include <string>
#include <vector>
#include <stdexcept>
#include <list>
#include <memory>
#include <algorithm>
#include <assert.h>
#include <atomic>
#include <sstream>
#include <iostream>

// remove these following two #defines once the tensor lib works
#define ENABLE_TENSORVIEW                // if set then tensor lib is used instead of old Matrix implementations, wherever such an implementation exists
#define ENABLE_BROADCASTING_ELEMENTTIMES // if set then ScaleNode and Row/ColumnElementTimes are redirected to ElementTimes

#define DEFAULT_HIDDEN_ACTIVATION 0.1

#pragma warning(disable : 4267) // conversion from size_t to int or other types

// version number to control how to read and write 
#define CNTK_MODEL_VERSION_1 1
#define CNTK_MODEL_VERSION_2 2
#define CURRENT_CNTK_MODEL_VERSION 2

extern bool g_shareNodeValueMatrices;

#ifndef UNREFERENCED_PARAMETER
#define UNREFERENCED_PARAMETER(P) (P)
#endif

// helper mode for debugging
// If TRACK_GAP_NANS is defined then initialize layout gaps to NaN and do NaN checks. Also do detailed logging of node computations.
// #define TRACK_GAP_NANS

namespace Microsoft { namespace MSR { namespace CNTK {

enum CopyNodeFlags // flags to be passed to the CopyTo() function
    {
    copyNodeNull = 0,                 // invalid value
    copyNodeValue = 1,                // copy everything but the children links
    copyNodeChildren = 2,             // only copy over children links
    copyNodeAll = 3,                  // copy everything
    copyNodeChildrenCrossNetwork = 4, // allow a cross network child copy
    };

#pragma region base computation class

    // =======================================================================
    // IComputationNode -- set of methods that are to be implemented (or optionally overridable) by node implementations.
    // =======================================================================

    class ComputationNodeBase;
struct /*interface*/ IComputationNode
    {
        typedef shared_ptr<ComputationNodeBase> ComputationNodeBasePtr;

        // --- these must be implemented by each node

    virtual ComputationNodeBase* NewThis(DEVICEID_TYPE deviceId, const wstring& name) = 0;
        // TODO: OperationName calls static TypeName which does not match the actual type names in that the 'Node' is missing.
        virtual const std::wstring OperationName() const = 0;
#define OperationNameOf(T) (T<float>::TypeName()) // convenience macro

    virtual void UpdateFunctionMBSize() = 0; // recalculate our column dimensions from MBLayout. Override to update temps.

    virtual void BeginForwardProp() = 0;             // called beforefirst iteration step of ForwardProp()
    virtual void ForwardProp(const FrameRange&) = 0; // forward prop for one minibatch
    virtual void EndForwardProp() = 0;               // called after last iteration step of ForwardProp()

    virtual void BeginBackprop() = 0;                                        // called before first iteration step of ComputeGradient()
    virtual void BackpropTo(const size_t inputIndex, const FrameRange&) = 0; // backprop gradient into one of the inputs
    virtual void EndBackprop() = 0;                                          // called after last iteration step of ComputeGradient()

        // --- these are meant to be overridden by ControlFlowNodes

    virtual void Backprop(const FrameRange& fr, bool childrenInThisLoop, bool childrenInOuterLoop) = 0;

        // --- optional overrides that add functionality

        // Any override must call Base version as well.
        // Default implementations are in ComputationNodeBase or ComputationNode<ElemType>.

    virtual void Validate(bool isFinalValidationPass) = 0; // main base validation function
        virtual void Save(File& fstream) const = 0;
        virtual void Load(File& /*fstream*/, size_t /*modelVersion*/) = 0;
        virtual void CopyTo(ComputationNodeBasePtr node, const std::wstring& newName, const CopyNodeFlags flags) const = 0;

    virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool) = 0; // request matrices needed to do node function value evaluation
    virtual void ReleaseMatricesAfterForwardProp(MatrixPool& matrixPool) = 0;  // release temp matrices that are only used by forward computation. Don't release matrices that need to be used in the gradient computation
        virtual void AllocateGradientMatricesForInputs(MatrixPool& matrixPool) = 0;
    virtual void RequestMatricesBeforeBackprop(MatrixPool& matrixPool) = 0; // request matrices that are needed for gradient computation
    virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool) = 0;  // release gradient and temp matrices that no longer needed after all the children's gradients are computed.

        // --- optional overrides that describe a feature or property of the node

    virtual bool RequiresPreCompute() const = 0; // return true if the node's value should be computed before the normal training. e.g., mean and invStd of input features.

        // --- optional overrides for more informative logging

    virtual void PrintSelfBeforeValidation() const = 0; // called in validation loop right before Validate()
        virtual void DumpNodeInfo(const bool /*printValues*/, File& fstream) const = 0;

    protected:
    virtual ~IComputationNode()
    {
    }
    };

    // =======================================================================
    //  This provide a interface for stateful node (e.g., DelayNodeBase) and definition of state
    //  This interface allows to Export and Import state from elsewhere 
    //  It is needed when doing sub-minibatch implementation 
    // =======================================================================

class INodeState : public std::enable_shared_from_this<INodeState>
    {
    public:
    virtual ~INodeState()
    {
    }
    };

    struct /*interface*/ IStatefulNode
    {
        typedef std::shared_ptr<INodeState> NodeStatePtr;
        virtual NodeStatePtr ExportState() = 0;
    virtual void ImportState(const NodeStatePtr& state) = 0;
    };
    typedef IStatefulNode::NodeStatePtr NodeStatePtr;

    // =======================================================================
    // ComputationNetworkOwnedNodeState -- class to collect ComputationNode members that are really owned by ComputationNetwork
    // These members are only to be set, changed, and read by ComputationNetwork code.
    // =======================================================================

    class ComputationNetwork;
    struct ComputationNetworkOwnedNodeState
    {
        friend class ComputationNetwork;

    ComputationNetworkOwnedNodeState()
        : m_needsGradient(false), m_valueSharable(true)
        {
            PurgeStateForFormingRecurrentLoops();
            m_isPartOfLoop = false;
        }

    void CopyTo(ComputationNetworkOwnedNodeState& other) const
        {
            // TODO: is that really all we copy? (this is a result of refactoring, so it seems yes indeed). Should we at least ClearCache()?
            other.m_isPartOfLoop = m_isPartOfLoop;
            other.m_needsGradient = m_needsGradient;
        }

    bool IsPartOfLoop() const
    {
        return m_isPartOfLoop;
    }

    virtual void MarkValueNonSharable()
    {
        m_valueSharable = false;
    }
    virtual void MarkValueSharable()
    {
        m_valueSharable = true;
    }
    bool isValueSharable() const
    {
        return m_valueSharable;
    }
        
protected:                // TODO: should be fully encapsulated here
    bool m_needsGradient; // true if this node or any children need a gradient to be computed (for own consumption or propagation to somewhere in the child tree)

    bool m_valueSharable; // a flag is needed for memory share.
                                // If it is false (e.g., learnableParameters/InputValue and those nodes are solely induced by learnableParameters), 
                                // it will never be released to memory pool 
    private:
    bool m_isPartOfLoop; // true if this loop is part of a recurrent loop

    protected:
        // owned by FormRecurrentLoops() and stuff it calls, only used from inside there (FormRecurrentLoops() calls PurgeStateForFormingRecurrentLoops() at its end to make that super-clear)
        void PurgeStateForFormingRecurrentLoops()
        {
            m_loopId = -1;
            m_visitedOrder = -1;
            m_indexInLoop = 0;
            m_visited = false;
            m_index = -1;
            m_minIndex = -1;
            m_inStack = false;
        }

    int m_loopId;       // index into m_allSEQNodes array, for use by reordering operation only
    int m_visitedOrder; // remembers order in which nodes were visited by EnumerateNodes(), but gets updated
    bool m_visited;     // note: also used by ValidateSubNetwork()
        int m_indexInLoop;
        // only used inside DetermineSCCs():
    int m_index;    // index denoting order in which nodes were visited in DetermineSCCs()
    int m_minIndex; // min of m_index over all nodes within a single loop
        bool m_inStack;
    };

    // =======================================================================
    // TimeStamp -- helper class to manage a "time stamp" (unique value) of a computation result to avoid recomputation
    // =======================================================================

    class TimeStamp
    {
    public:
    TimeStamp()
    {
        ResetEvalTimeStamp();
    }
    void CopyTo(TimeStamp& other) const
    {
        other.m_evalTimeStamp = m_evalTimeStamp;
    }
    void ResetEvalTimeStamp()
    {
        m_evalTimeStamp = s_timeStampCounter;
    }
    int64_t GetEvalTimeStamp() const
    {
        return m_evalTimeStamp;
    }

        // create a new unique time stamp
    void BumpEvalTimeStamp()
    {
        m_evalTimeStamp = CreateUniqId();
    }

        // the difference is taken to take into account numeric overflow (which really should never happen for a 64-bit integer... but hey, it's free!)
    bool IsOlderThan(const TimeStamp& other) const
        {
            // BUGBUG: For some reason, we must test equality as well, although that does not indicate being older.
            return GetEvalTimeStamp() - other.GetEvalTimeStamp() /*<*/ <= 0;
        }

        int64_t CreateUniqId() const
        {
            return /*1 +*/ atomic_fetch_add(&s_timeStampCounter, (unsigned long long int) 1);
        }

    private:
        static atomic_ullong s_timeStampCounter;
        int64_t m_evalTimeStamp; //this is used to reduce unnecessary recomputation when a different node in the model is reevaluated
    };

    // =======================================================================
    // ComputationNodeBase -- abstract base class for all computation nodes
    // =======================================================================

class ComputationNodeBase : public IComputationNode,
                            public /*protected*/ ComputationNetworkOwnedNodeState, // TODO: figure the 'protected' business out, somehow the 'friend' thing does not work
                            public TimeStamp,                                      // for time-stamp management
        public ScriptableObjects::ComputationNodeObject,
                            public ScriptableObjects::WithTag,
                            public ScriptableObjects::HasName,
                            public ScriptableObjects::HasToString,
        public std::enable_shared_from_this<ComputationNodeBase>
    {
        // note: enable_shared_from_this<> allows to create a shared_ptr from a raw pointer to this that is correctly aware of all other shared_ptrs (same ref count)
    public:
        typedef shared_ptr<ComputationNodeBase> ComputationNodeBasePtr;

    ComputationNodeBase(DEVICEID_TYPE deviceId, const wstring& name)
        : m_deviceId(deviceId), m_outputNeededDuringBackprop(true), m_parameterUpdateRequired(false), m_gradientInitialized(false), m_nodeName(name == L"" ? CreateUniqNodeName() : name)
    {
    }
    virtual ~ComputationNodeBase()
    {
    }

        virtual void CopyTo(ComputationNodeBasePtr node, const std::wstring& newName, const CopyNodeFlags flags) const
        {
            if (OperationName() != node->OperationName())
                RuntimeError("Cannot copy from one node type to another node type");
            if (flags & CopyNodeFlags::copyNodeChildren)
            {
                node->m_inputs = m_inputs;
            }
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                node->m_deviceId = m_deviceId;
                node->m_parameterUpdateRequired = m_parameterUpdateRequired;
                node->m_nodeName = newName;

                node->m_sampleLayout = m_sampleLayout;

                ComputationNetworkOwnedNodeState::CopyTo(*node);
                TimeStamp::CopyTo(*node);
            }
        }

        virtual ComputationNodeBasePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags) = 0;

        // TODO: make sure this does not get implemented in any of the base classes
    DEVICEID_TYPE GetDeviceId() const
    {
        return m_deviceId;
    } // TODO: remove, only used from copy constructor which will go away

        virtual void Save(File& fstream) const
        {
            fstream << OperationName() << NodeName();
        }

        virtual void Load(File& /*fstream*/, size_t /*modelVersion*/)
        {
            // it is assumed that OperationName and NodeName have already been consumed--some asymmetry between Save and Load
            // base class has nothing to load
        }

        // dimensions

        // The value of a node is a tensor in one of two variants:
        //
        //  - single matrix, vector, tensor
        //     - m_sampleLayout contains the shape. Accessed through GetSampleLayout().
        //     - m_pMBLayout is null
        //  - minibatch data
        //     - consists of many samples which are all tensors of m_sampleLayout
        //     - adds two additional tensor dimensions, time step and parallel sequence
        //       These change for each minibatch and are unknown during validation.
        //     - m_sampleLayout is the tensor shape of the samples
        //     - m_pMBLayout defines the number of time steps and parallel sequences (="tensor shape" of the minibatch)
        //       Accessed through GetMBLayout(); test for through HasMBLayout().
        //
        // The values can be accessed in three ways:
        //
        //  - as a tensor
        //     - GetTensorShape() forms the joint tensor that incorporates both m_sampleLayout and, if present, m_pMBLayout
        //        - Elementwise tensor operations operate on these.
        //        - If no MBLayout is present in one of multiple elementwise operands, it will be interpreted as a one-sample minibatch that broadcasts to all samples.
        //     - learnable parameters hold tensors that are not minibatches
        //  - as a sample matrix
        //     - many nodes do not care about the specific sample-tensor dimensions
        //     - but may care about selecting a single time step out of a minibatch
        //     - minibatch: each matrix column contains a sample tensor flattened, with one column per time step and parallel sequence
        //     - tensor: one column containing the sample tensor flattened
        //     - GetSampleMatrixNumRows(), GetSampleMatrixNumCols()
        //  - as a Matrix reference
        //     - actual object is a 2D tensor without MB Layout
        //     - ValueAsMatrix(), GradientAsMatrix() returns tensor as a 2D Matrix object
        //     - nodes that do this are: TimesNode, DiagTimesNode, ConvolutionNode, NoiseContrastiveEstimationNode, ClassBasedCrossEntropyWithSoftmaxNode, TransposeNode, DiagonalNode
        //
        // How values are stored:
        //
        //  - minibatch: Matrix of columns, where each column is a sample
        //  - tensor: Matrix where column dimension contains all but the first dimension
        //     - This only matters for sparse matrices, which cannot easily be Reshaped().
        //       For those, we keep the underlying storage identical to the semantic meaning.

        // interpretation as a set of samples
        const TensorShape& GetSampleLayout() const { return m_sampleLayout; }
        bool HasSampleLayout() const { return m_sampleLayout.GetRank() != 1; }      // does it have a layout that is not just a vector?

        // interpretation as sample matrix (each column is a sample, individual sample tensor dimensions do not matter for the operation)
        size_t GetSampleMatrixNumRows() const
        {
            return m_sampleLayout.GetNumElements();
        }
        size_t GetSampleMatrixNumCols() const
        {
            if (HasMBLayout())
                return GetMBLayout()->GetNumCols();
            else
                return 1;   // no layout: treat as 1-sample minibatch that is meant to broadcast
        }
        // determine if we are the output of an op over 'other', whether that would be a reduction, so that we need to mask
        bool ReducesInTimeWrt(const ComputationNodeBasePtr & other) const
        {
            return GetSampleMatrixNumCols() < other->GetSampleMatrixNumCols();
        }

        // interpretation as a Matrix reference
    private:
        void CheckTensorIsMatrix() const
        {
            if (HasMBLayout())
                LogicError("CheckTensorIsMatrix: Minibatch data cannot be interpreted as a single 2D tensor.");
            else if (m_sampleLayout.GetRank() < 1 || m_sampleLayout.GetRank() > 2)  // note: scalars are not stored as tensors of rank 0, but rather as 1-dim vectors. TODO: clean this up some day
                LogicError("CheckTensorIsMatrix: Sample is not a column vector or matrix (1D or 2D tensor).");
        }
    public:
        size_t GetAsMatrixNumRows() const
        {
            CheckTensorIsMatrix();
            return m_sampleLayout[0];
        }
        size_t GetAsMatrixNumCols() const
        {
            CheckTensorIsMatrix();
            return m_sampleLayout.GetRank() > 1 ? m_sampleLayout[1] : 1;    // a column vector is also a Matrix
        }

        // set dimensions of the node
        // The MBLayout must be set first, and 'isMinibatch' will be checked against it.
        void SetDims(const TensorShape& sampleLayout, bool isMinibatch)
        {
            if (HasMBLayout() != isMinibatch)
                LogicError("SetDims: MBLayout must be set first, before calling this function, for %ls %ls operation.", NodeName().c_str(), OperationName().c_str());
            m_sampleLayout = sampleLayout;
        }
        // copy dimensions (rows, cols, sample layout) from another node
        void SetDims(const ComputationNodeBasePtr& node)
        {
            SetDims(node->GetSampleLayout(), node->HasMBLayout());
        }
        // use this only for testing code. Everywhere else, be explicit on the TensorShape.
        void SetDims1(size_t rows, size_t cols)
        {
            SetDims(TensorShape(rows, cols), false);
        }
#if 0
        // deprecated functions that did not distinguish the purpose
        size_t GetNumRows() const { return GetSampleMatrixNumRows(); }
        size_t GetNumCols() const
        {
            if (HasMBLayout() && GetNumMBCols() != m_numCols)
                LogicError("GetNumCols: %ls %ls operation: Inconsistency between m_numCols (%d) and MBLayout (%d)", NodeName().c_str(), OperationName().c_str(), m_numCols, (int)GetNumMBCols());
            else if (!HasMBLayout() && m_sampleLayout.GetRank() == 0 && m_numCols != 0)
                LogicError("GetNumCols: %ls %ls operation: Inconsistency between m_numCols (%d) and sample layout (empty)", NodeName().c_str(), OperationName().c_str(), (int)m_numCols);
            else if (!HasMBLayout() && m_sampleLayout.GetRank() > 0 && m_numCols != m_sampleLayout.GetDims().back())
                LogicError("GetNumCols: %ls %ls operation: Inconsistency between m_numCols (%d) and last dim of sample layout [%s]", NodeName().c_str(), OperationName().c_str(), (int)m_numCols, string(m_sampleLayout).c_str());
            return m_numCols;
        }
        size_t GetNumCols1() const { return m_numCols; }
        // update number of columns (in response to MB size)
        // TODO: this should go away, as m_numCols should be derived from MBLayout each time
        void SetNumCols(size_t cols)
        {
            if (!HasMBLayout())
                LogicError("SetNumCols: %ls %ls operation has no MBLayout.", NodeName().c_str(), OperationName().c_str());
            if (cols != m_pMBLayout->GetNumCols())
                LogicError("SetNumCols: %ls %ls operation: SetNumCols() is redundant with MBLayout %d, but got differing value %d.", NodeName().c_str(), OperationName().c_str(), (int)m_pMBLayout->GetNumCols(), (int)cols);
            // TODO: replace by a check
            m_numCols = cols;
            // actual memory allocation happens elsewhere
        }
#endif
        // get number of underlying matrix columns for test code only which does not create MBLayouts
        size_t GetNumCols1() const { return GetSampleMatrixNumCols(); }    // dummy
        virtual void NotifyFunctionValuesMBSizeModified() = 0;
        void VerifyDims(const TensorShape & shape, bool isMinibatch)
        {
            if (m_sampleLayout.GetDims() != shape.GetDims() || HasMBLayout() != isMinibatch)
            {
                LogicError("VerifyDims: %ls %ls operation expected a %s of [%s], but it is a %s of [%s]",
                           NodeName().c_str(), OperationName().c_str(),
                           isMinibatch ?   "minibatch" : "tensor", string(shape).c_str(),
                           HasMBLayout() ? "minibatch" : "tensor", string(m_sampleLayout).c_str());
            }
        }
        virtual void VerifyDims(ComputationNodeBasePtr node)
        {
            VerifyDims(node->GetSampleLayout(), node->HasMBLayout());
        }

    TensorShape GetTensorShape(size_t rank) const; // form the actual tensor that describes the full object
protected:
    size_t DetermineElementwiseTensorRank() const;                          // determine tensor rank when considering all inputs with padding
    TensorShape GetTensorSliceFor(size_t rank, const FrameRange& fr) const; // form tensor shape of the slice referenced by FrameRange
    public:
        // access to element(0,0) without having to type-cast
        virtual double Get00Element() const = 0;

        // validation
        // This is overridden by every node. This base class just checks for unconnected and empty inputs. Overrides must call their base version first.
        virtual void Validate(bool isFinalValidationPass) // main base validation function
        {
            // check for NULL pointers
            for (size_t i = 0; i < m_inputs.size(); i++)
            {
                if (!m_inputs[i])
                    RuntimeError("Validate: Input [%d] of %ls node '%ls' is empty (NULL, not connected).", (int) i, OperationName().c_str(), NodeName().c_str());
            }
            // check for empty inputs
            if (isFinalValidationPass)
            {
                for (const auto& child : m_inputs)
                    if (child->GetSampleMatrixNumRows() == 0)
                        RuntimeError("%ls %ls operation: input %ls %ls has 0 elements.", NodeName().c_str(), OperationName().c_str(), child->NodeName().c_str(), child->OperationName().c_str());
            }
        }
        // helper functions for common cases
    protected:
        void ValidateUnaryMap(bool isFinalValidationPass);
        void ValidateUnaryReduce(bool isFinalValidationPass);
        void ValidateInferBinaryInputDims();
        void ValidateBinaryZip(bool isFinalValidationPass, bool allowMultiples);
        void ValidateBinaryReduce(bool isFinalValidationPass);

    public:
    virtual bool UnitTest()
    {
        return true;
    }

        virtual void AttachInputs(const std::vector<ComputationNodeBasePtr>& inputs) = 0;
        // convenience versions that take individual arguments
    void AttachInputs(const ComputationNodeBasePtr& singleInput)
    {
        AttachInputs(std::vector<ComputationNodeBasePtr>{singleInput});
    }
    void AttachInputs(const ComputationNodeBasePtr& leftInput, const ComputationNodeBasePtr& rightInput)
    {
        AttachInputs(std::vector<ComputationNodeBasePtr>{leftInput, rightInput});
    }
    void AttachInputs(const ComputationNodeBasePtr& leftInput, const ComputationNodeBasePtr& middleInput, const ComputationNodeBasePtr& rightInput)
    {
        AttachInputs(std::vector<ComputationNodeBasePtr>{leftInput, middleInput, rightInput});
    }
    void AttachInputs(const ComputationNodeBasePtr& firstInput, const ComputationNodeBasePtr& secondInput, const ComputationNodeBasePtr& thirdInput, const ComputationNodeBasePtr& fourthInput)
    {
        AttachInputs(std::vector<ComputationNodeBasePtr>{firstInput, secondInput, thirdInput, fourthInput});
    }
    void AttachInputs(const ComputationNodeBasePtr& firstInput, const ComputationNodeBasePtr& secondInput, const ComputationNodeBasePtr& thirdInput, const ComputationNodeBasePtr& fourthInput, const ComputationNodeBasePtr& fifthInput)
    {
        AttachInputs(std::vector<ComputationNodeBasePtr>{firstInput, secondInput, thirdInput, fourthInput, fifthInput});
    }
    void AttachInputs(const ComputationNodeBasePtr& firstInput, const ComputationNodeBasePtr& secondInput, const ComputationNodeBasePtr& thirdInput, const ComputationNodeBasePtr& fourthInput, const ComputationNodeBasePtr& fifthInput, const ComputationNodeBasePtr& sixthInput)
    {
        AttachInputs(std::vector<ComputationNodeBasePtr>{firstInput, secondInput, thirdInput, fourthInput, fifthInput, sixthInput});
    }

    virtual void DetachInputs()
    {
        m_inputs.clear();
    }

        // helper for the factory function for ComputationNodes
        static vector<ComputationNodeBasePtr> GetInputsFromConfig(const ScriptableObjects::IConfigRecordPtr configp)
        {
            vector<ComputationNodeBasePtr> inputs;
        const auto* inputsArg = configp->Find(L"inputs");
            if (inputsArg)
            {
            if (inputsArg->Is<ComputationNodeBase>()) // single arg
                    inputs.push_back(*inputsArg);
            else // a whole vector
                {
                    ScriptableObjects::ConfigArrayPtr inputsArray = *inputsArg;
                    const auto range = inputsArray->GetIndexRange();
                for (int i = range.first; i <= range.second; i++) // pull them. This will resolve all of them.
                    inputs.push_back(inputsArray->At(i, [](const wstring&)
                                                     {
                                                         LogicError("GetInputs: out of bounds index while iterating??");
                                                     }));
                }
            }
            return inputs;
        }

    const std::vector<ComputationNodeBasePtr>& GetInputs() const
    {
        return m_inputs;
    }
    const ComputationNodeBasePtr& Input(size_t index) const
    {
        return m_inputs[index];
    }

        //return true if the node's value should be computed before the normal training. e.g., mean and invStd of input features.
    virtual bool /*IComputationNode::*/ RequiresPreCompute() const
    {
        return false;
    }

        // casting helpers
    template <typename N>
    N* As()
        {
            auto p = dynamic_cast<N*>(this);
            if (!p)
                LogicError("Attempted to type-cast node %ls %ls to %s, which is not possible.", NodeName().c_str(), OperationName().c_str(), typeid(N).name());
            return p;
        }
    template <typename N>
    bool Is()
    {
        return dynamic_cast<N*>(this) != nullptr;
    }

    /*HasName::*/ void SetName(const std::wstring& newName) // also for use by ExperimentalNetworkBuilder
        {
            m_nodeName = newName;
            fprintf(stderr, "Node --> %ls = %ls\n", NodeName().c_str(), OperationName().c_str()), fflush(stderr);
        }

    void LinkToMBLayout(MBLayoutPtr pMBLayout)
    {
        m_pMBLayout = pMBLayout;
    }
    const MBLayoutPtr& GetMBLayout() const
    {
        return m_pMBLayout;
    }
    bool HasMBLayout() const
    {
        return !!m_pMBLayout;
    }

    std::wstring GetName() const
    {
        return m_nodeName;
    }

        // temporary function that is called to verify stuff is called as I think it is. Delete if this does not fire for a while.
        void VerifyNumParallelSequences(size_t bsz)
        {
            if (bsz != m_pMBLayout->GetNumParallelSequences())
                LogicError("VerifyNumParallelSequences: value inconsistent with MB layout");
        }

    protected:
public: // ...the following should be protected, but nodes inquire about their children, requiring public access
        size_t GetNumParallelSequences() const
        {
#if 1
        if (!m_pMBLayout) // TODO: temporary workaround to Check_t() calls which call this. TODO: Delete the first arg from Check_t() after memshare merge.
                return SIZE_MAX;
#endif
            return m_pMBLayout->GetNumParallelSequences();
        }

        // get our current number of time steps for this node
        // This inquires the MB layout.
        size_t GetNumTimeSteps() const
        {
            if (!m_pMBLayout)
                LogicError("GetNumTimeSteps: invalid to call on a node without MB layout"); // since it has no notion of time
            return m_pMBLayout->GetNumTimeSteps();
        }

public:
        // implemented by ComputationNode<ElemType>
        // for debugging purpose
        virtual void PrintSelf(bool printMatrices = false) const = 0;

        // called in validation loop right before Validate()
        virtual void /*IComputationNode::*/ PrintSelfBeforeValidation() const
        {
            fprintf(stderr, "\nValidating --> %ls = %ls", NodeName().c_str(), OperationName().c_str());

            if (!IsLeaf())
            {
                fprintf(stderr, "(");
            for (size_t i = 0; i < GetNumInputs(); i++)
                {
                const auto& child = m_inputs[i];
                    if (i > 0)
                        fprintf(stderr, ", ");

                    if (child == nullptr)
                    {
                        fprintf(stderr, "NULL");
                        continue;
                    }

                    const char* mbSizeMark = child->m_pMBLayout ? " x *" : "";
                    if (child->m_sampleLayout.GetRank() == 3 && (child->m_sampleLayout[1] != 1 || child->m_sampleLayout[0] != 1)) // looks like an image: use WHC notation
                        fprintf(stderr, "%ls[%s%s {W=%lu, H=%lu, C=%lu}]", child->NodeName().c_str(), string(child->m_sampleLayout).c_str(), mbSizeMark,
                                child->m_sampleLayout[1], child->m_sampleLayout[2], child->m_sampleLayout[0]);
                    // BUGBUG: This ^^ will print based on the old legacy layout, and we have no way of knowing here whether that is correct.
                    else
                        fprintf(stderr, "%ls[%s%s]", child->NodeName().c_str(), string(child->m_sampleLayout).c_str(), mbSizeMark);
                }
                fprintf(stderr, ")");
            }
        }

    const std::wstring& NodeName() const
    {
        return m_nodeName;
    }
    void SetNodeName(const std::wstring& nodeName)
    {
        m_nodeName = nodeName;
    }

    bool IsLeaf() const
    {
        return GetNumInputs() == 0;
    }
    bool& NeedGradient()
    {
        return m_needsGradient;
    }
    const bool& NeedGradient() const
    {
        return m_needsGradient;
    }

    void SetParameterUpdateRequired(bool f)
    {
        m_parameterUpdateRequired = f;
    }
    bool IsParameterUpdateRequired() const
    {
        return m_parameterUpdateRequired;
    }

    void SetOutputNeededDuringBackprop(bool f)
    {
        m_outputNeededDuringBackprop = f;
    }
        bool IsOutputNeededDuringBackprop() const 
        {
        return !g_shareNodeValueMatrices || m_outputNeededDuringBackprop;
        }

    const size_t GetNumInputs() const
    {
        return m_inputs.size();
    }

        virtual void SetInput(const size_t childIndex, const ComputationNodeBasePtr& node) = 0;

        // masking
        // overridden by <ElemType> variant only
    virtual void MaskMissingValueColumnsToZero(const FrameRange&) = 0;
    virtual void MaskMissingGradientColumnsToZero(const FrameRange&) = 0;
    virtual void InvalidateMissingValueColumns(const FrameRange&) = 0;
    virtual void InvalidateMissingGradientColumns(const FrameRange&) = 0;

        virtual void ZeroGradientsOfInputs() = 0;

    virtual void /*IComputationNode::*/ BeginForwardProp() override // called before first iteration step of ForwardProp()
        {
#ifdef TRACK_GAP_NANS
            fprintf(stderr, "BeginForwardProp: %ls %ls operation\n", NodeName().c_str(), OperationName().c_str());
#endif
        }
    virtual void /*IComputationNode::*/ EndForwardProp() override // called after last iteration step of ForwardProp()
        {
#ifdef TRACK_GAP_NANS
            fprintf(stderr, "EndForwardProp: %ls %ls operation\n", NodeName().c_str(), OperationName().c_str());
#endif
        }
        // TODO: the following two are not really utilized yet other than printing trace information
    virtual void /*IComputationNode::*/ BeginBackprop() override // called before first iteration step of ComputeGradient()
        {
#ifdef TRACK_GAP_NANS
            fprintf(stderr, "BeginBackprop: %ls %ls operation\n", NodeName().c_str(), OperationName().c_str());
#endif
        }
    virtual void /*IComputationNode::*/ EndBackprop() override // called after last iteration step of ComputeGradient()
        {
#ifdef TRACK_GAP_NANS
            fprintf(stderr, "EndBackprop: %ls %ls operation\n", NodeName().c_str(), OperationName().c_str());
#endif
        }

        // Is the output value of the computation node needed for computing 
        // gradients of any of the input nodes
        // Base-class version makes conservative assumption that it is. Override if not.
        virtual bool OutputUsedInComputingInputNodesGradients() const
        {
            return true;
        }

        // Is the output value of the specified  input node needed for computing
        // gradients of any of the input nodes
        // Base-class version makes conservative assumption that it is. Override if not.
        virtual bool InputUsedInComputingInputNodesGradients(size_t childIndex) const
        {
            UNREFERENCED_PARAMETER(childIndex);
            return true;
        }

    public:
        virtual void ValidateInferInputDimsFrom(const TensorShape &) = 0;

    protected:
    const TensorShape& GetInputSampleLayout(const size_t index) const
        {
            return m_inputs[index]->GetSampleLayout();
        }

        void InferMBLayoutFromInputsForStandardCase();

    public:
        bool IsEqualTo(const ComputationNodeBasePtr& other) const //this will be used to determine whehter two nodes are the same
        {
            if (OperationName() != other->OperationName() || m_inputs.size() != other->m_inputs.size())
                return false;

        if (NodeName() == other->NodeName()) //assume names are unique in the system
                return true;

        if (IsLeaf() && other->IsLeaf()) //since names are not equal otherwise will return above
                return false;

        for (size_t i = 0; i < m_inputs.size(); i++)
                if (!(m_inputs[i] == other->m_inputs[i]))
                    return false;

            return true;
        }

        // determine enumeration order for everything needed to evaluate this node (and its children)
        // This creates a list such that children are evaluated before their parents.
        // If !forForwardProp then the order will be reversed, suitable for backprop.
        // The 'recurrent' version is only called from FormRecurrentLoops().
        // TODO: This should be a method of ComputationNetwork, not ComputationNode.
    static std::list<ComputationNodeBasePtr> EnumerateNodes(const std::vector<ComputationNodeBasePtr>& allRoots, bool skipPairNetwork = false /*legacy*/)
        {
            std::list<ComputationNodeBasePtr> nodes;
            std::unordered_set<ComputationNodeBasePtr> visited;

        for (const auto& root : allRoots)
            root->EnumerateNodesRec(visited, nodes, skipPairNetwork); // call into the recursive portion of this function below

            return nodes;
        }

        // and a version that does it for only one root 'this'
    std::list<ComputationNodeBasePtr> EnumerateNodes(bool skipPairNetwork) /*const*/
    {
        return EnumerateNodes(std::vector<ComputationNodeBasePtr>{shared_from_this()}, skipPairNetwork);
    }

    private:
        // Recursive part of EnumerateNodes().
        void EnumerateNodesRec(std::unordered_set<ComputationNodeBasePtr>& visited, std::list<ComputationNodeBasePtr>& result, bool skipPairNetwork) /*const*/ // const not working due to shared_from_this()
        {
        if (visited.find(shared_from_this()) == visited.end()) // do not include a node twice
            {
            visited.insert(shared_from_this()); // have visited tagged here to avoid infinite loop over children, children's children, etc

                // children first for function evaluation
            if (OperationName() != L"PairNetwork" || !skipPairNetwork) // (don't step through network-pair boundary if called from FormRecurrentLoops())
                {
                    for (int i = 0; i < m_inputs.size(); i++)
                    {
                        if (m_inputs[i])
                            m_inputs[i]->EnumerateNodesRec(visited, result, skipPairNetwork);
                    }
                }

                // now that all children are in list before us, put ourselves
                result.push_back(shared_from_this());
            }
        }

public:
        // check whether a node is up-to-date w.r.t. its children, for lazy evaluation
        // If this returns false, node must be evaluated to update m_value.
        // BUGBUG: The function name is incorrect. It also returns 'true' if a child has the same time stamp (not older).
        // This is virtual because it is overridden by traversal nodes.
        virtual bool IsOutputOlderThanInputs() const
        {
            // TODO: use range-based for
            for (size_t i = 0; i < GetNumInputs(); i++)
            {
                if (IsOlderThan(*m_inputs[i]))
                    return true;
            }

            return false;
        }

        typedef std::pair<ComputationNodeBasePtr, ComputationNodeBasePtr> ComputationArc;
        // [1/13/2015 erw] add to enumerate all the edges 
        // enumerate arcs that can be reached starting from the current node's children
        // [in/out] visited record already visited nodes 
        // TODO: This should be a method of ComputationNetwork, not ComputationNode.
        void EnumerateArcs(std::unordered_set<ComputationNodeBasePtr>& visited, std::list<ComputationArc>& arcs)
        {
        std::list<ComputationNodeBasePtr> tovisit;

            if (visited.find(shared_from_this()) == visited.end()) // only do when this node has not been visited before
            {
                tovisit.push_back(shared_from_this());

                while (!tovisit.empty())
                {
                    ComputationNodeBasePtr curNode = tovisit.front();
                    tovisit.pop_front();

                    if (visited.find(curNode) == visited.end())
                    {
                        for (size_t i = 0; i < curNode->m_inputs.size(); i++)
                        {
                            arcs.push_back(ComputationArc(curNode, curNode->m_inputs[i]));

                            if (visited.find(curNode->m_inputs[i]) == visited.end()) // this children has not been visited before 
                            tovisit.push_front(curNode->m_inputs[i]);            // going to visit each of the children
                        }
                        visited.insert(curNode);
                    }
                }
            }
        }

        std::wstring CreateUniqNodeName() const
        {
#ifdef USE_GUID_AS_NAME
            UUID uuid;
            ZeroMemory(&uuid, sizeof(UUID));
            std::wstring name;

            UuidCreate(&uuid);
            WCHAR* szUuid = nullptr;
        if (UuidToStringW(&uuid, (RPC_WSTR*) &szUuid) != RPC_S_OK)
                RuntimeError("Failed to craete unique node name.");
            else
            {
                name = szUuid;
            RpcStringFreeW((RPC_WSTR*) &szUuid);
            }
#else
            int64_t id = CreateUniqId();
            std::wstring base = L"AutoName";
            std::wstringstream sstm;
            sstm << base.c_str() << id;
            std::wstring name = sstm.str();
            //msra::strfun::wstrprintf name(L"%s%d", L"AutoName", id);
#endif

            return name;
        }

    protected:
    DEVICEID_TYPE m_deviceId; // CPU=-1, >=0 GPU
        std::wstring m_nodeName;

        // inputs
        std::vector<ComputationNodeBasePtr> m_inputs;

        // dimensions and layout
        // Data is stored as a Matrix object, but often it is interpreted as a tensor.
        // For nodes that carry data (samples), each sample is a column of the matrix, which is interpreted as
        // a tensor (n-dimensional array) described by m_sampleLayout. The MBLayout describes the meaning
        // of the column index.
        // For nodes that do not carry data, the last tensor index of m_sampleLayout is the number of columns.
        TensorShape m_sampleLayout; // sample layout
        MBLayoutPtr m_pMBLayout;

        // flags related to gradient propagation
    bool m_parameterUpdateRequired;    // update parameters? Only used for LearnableParameters.    --TODO: Should we make this a member of LearnableParameters actually? And require a type cast? Currently it is read out for all leaves.
    bool m_gradientInitialized;        // indicates whether the gradient matrix has been resized and initialized to 0
    bool m_outputNeededDuringBackprop; // indicates whether the output value of the node is needed during backprop
    };
    typedef ComputationNodeBase::ComputationNodeBasePtr ComputationNodeBasePtr;

    // =======================================================================
    // ComputationNode -- abstract base class for computation nodes, deriving from CompuationNodeBase, parameterized by float vs. double
    // =======================================================================

    // little helper class to allow derived Node classes to specify how many inputs they expect
struct INumInputs
{
    virtual size_t GetExpectedNumInputs() const = 0;
};
template <size_t m_numInputs>
struct NumInputs : public INumInputs
{
    size_t GetExpectedNumInputs() const override final
    {
        return m_numInputs;
    }
}; // e.g. derive from NumInputs<2>

template <class ElemType>
    class ComputationNode : public ComputationNodeBase // abstract class that cannot be instantiated
    {
        typedef ComputationNodeBase Base;

    protected:
        //std containers such as list and map does not support class reference so we need to use pointer
        typedef shared_ptr<ComputationNode<ElemType>> ComputationNodePtr;

    public:
    using ComputationNodeBase::AttachInputs; // import the convenience functions that take 1..6 parameters
        using ComputationNodeBase::SetDims;
        typedef ElemType OurElemType;

        // public constructor
        // Note: use the New<> helper function that is declared next, which gives you the convenience of returning a shared_ptr
    ComputationNode(DEVICEID_TYPE deviceId, const wstring& name)
        : ComputationNodeBase(deviceId, name)
    {
    }

        // creation from configuration
        // Nodes with NumInputs<> should say DeclareConstructorFromConfigWithNumInputs(ClassName), and nodes without DeclareConstructorFromConfig(ClassName).
        // The macro will forward to the regular constructor of the node (which may do more than just calling the base constructor), and then attach the inputs from config.
#define DeclareConstructorFromConfig(C)                  \
    C(const ScriptableObjects::IConfigRecordPtr configp) \
        : C(configp->Get(L"deviceId"), L"<placeholder>") \
    {                                                    \
        AttachInputs(configp);                           \
    }
#define DeclareConstructorFromConfigWithNumInputs(C)         \
    C(const ScriptableObjects::IConfigRecordPtr configp)     \
        : C(configp->Get(L"deviceId"), L"<placeholder>")     \
    {                                                        \
        AttachInputs(configp, this->GetExpectedNumInputs()); \
    }

        // helper to load m_value from a stream
        // This function updates the dimensions to a 2D matrix.
        // If a different tensor layout is associated with this, it must be implanted afterwards.
        // Nodes that call this never have an MB layout.
        void LoadValue(File& fstream)
        {
            CreateMatrixIfNull(m_value);
            fstream >> Value();
            // above reads dimensions, so we must update our own dimensions
            SetDims(TensorShape(Value().GetNumRows(), Value().GetNumCols()), false);
        }

        // reader updated m_functionValue and MBLayout--ensure our internal state is consistent
        virtual void NotifyFunctionValuesMBSizeModified() override final
        {
            if (!HasMBLayout())
                LogicError("NotifyFunctionValuesMBSizeModified: Must only be called on nodes with MBLayout.");
            if (GetSampleMatrixNumRows() != Value().GetNumRows())
                LogicError("NotifyFunctionValuesMBSizeModified: %ls %ls operation had its row dimension %d changed by the reader to %d.", NodeName().c_str(), OperationName().c_str(), (int)GetSampleMatrixNumRows(), (int)Value().GetNumRows());
            if (GetMBLayout()->GetNumCols() != Value().GetNumCols())
                LogicError("NotifyFunctionValuesMBSizeModified: %ls %ls operation had its col dimension %d changed by the reader to %d, but different from MBLayout.", NodeName().c_str(), OperationName().c_str(), (int)GetMBLayout()->GetNumCols(), (int)Value().GetNumCols());
        }
        virtual double Get00Element() const override final
        {
            // TODO: Are all these meant to read out a scalar? Then rename and verify dimensions.
            return Value().Get00Element();
        }

        // recover a shared_ptr from ourselves if given a naked pointer
        ComputationNodePtr shared_from_this()
        {
            return dynamic_pointer_cast<ComputationNode<ElemType>>(ComputationNodeBase::shared_from_this());
        }

        // recover a ComputationNodePtr (which is a shared_ptr) from a naked pointer to our base type (ComputationNodeBase) stored as a void* (old NDL parser does that)
    static ComputationNodePtr FromVoidPtr(void* vp)
        {
        auto p = dynamic_cast<ComputationNode<ElemType>*>((ComputationNodeBase*) vp); // TODO: check that all void* casts really come from ComputationNodeBasePtr; or add a method ToVoidPtr(). Or get rid of the void*?!
            return p->shared_from_this();
        }

        // AttachInputs() -- attach the inputs of a node
        // This verifies the number of inputs. For that, nodes with fixed number of inputs derive from NumInputs<N>.
        // This function discovers this through RTTI and performs a runtime check. Nodes should not have additional checks in their implementation (save the code).
        // Note: Nodes with variable number of inputs will not derive from NumInputs<>, but instead check their inputs in Validate().
        void AttachInputs(const std::vector<ComputationNodeBasePtr>& inputs)
        {
#ifdef _DEBUG
        wstring name = NodeName();
        name; // (for easier debugging)
#endif
        const auto* pNumInputs = dynamic_cast<INumInputs*>(this); // if this class also derives from NumInputs<N> then N is the expected number of inputs
            if (pNumInputs && pNumInputs->GetExpectedNumInputs() != inputs.size())
            RuntimeError("%ls operation '%ls' expects %d inputs (given: %d)", OperationName().c_str(), NodeName().c_str(), (int) pNumInputs->GetExpectedNumInputs(), (int) inputs.size());
            m_inputs.resize(inputs.size());
            for (size_t i = 0; i < m_inputs.size(); i++)
                if (inputs[i])
                m_inputs[i] = UpCast(inputs[i]); // (UpCast() checks the type; the assignment then downcasts it again)
                else
                m_inputs[i] = nullptr; // during network creation, nullpts are possible
        }

    protected:
        // AttachInputs() from config
        void AttachInputs(const ScriptableObjects::IConfigRecordPtr configp, size_t expectedNumInputs = SIZE_MAX)
        {
            const auto inputs = GetInputsFromConfig(configp);
            if (expectedNumInputs != SIZE_MAX)
            {
                if (inputs.size() != expectedNumInputs)
                {
                    // print an error. For that, find at least one argument
                auto* val = configp->Find(L"inputs");
                if (!val) // if there is no 'inputs' then get the first item of this config record for a Fail() function
                    {
                        auto members = configp->GetMemberIds();
                        if (members.size() > 0)
                            val = configp->Find(members.front());
                    }
                    if (val)
                    val->Fail(msra::strfun::wstrprintf(L"Expected %d inputs, but %d were given.", (int) expectedNumInputs, (int) inputs.size()));
                    else
                    InvalidArgument("Expected %d inputs, but %d were given.", (int) expectedNumInputs, (int) inputs.size());
                }
            }
            AttachInputs(inputs);
        }

    public:
        //request matrices needed to do node function value evaluation
        virtual void RequestMatricesBeforeForwardProp(MatrixPool& matrixPool)
        {
            RequestMatrixFromPool(m_value, matrixPool);
        }

        //release temp matrices that are only used by forward computation
        //don't release matrices that need to be used in the gradient computation
        virtual void ReleaseMatricesAfterForwardProp(MatrixPool& matrixPool)
        {
            if (!IsOutputNeededDuringBackprop() && (m_value->GetMatrixType() != SPARSE) && isValueSharable())
                ReleaseMatrixToPool(m_value, matrixPool);
        }

        virtual void AllocateGradientMatricesForInputs(MatrixPool& matrixPool) override
        {
            for (int i = 0; i < m_inputs.size(); i++)
            {
                if (m_inputs[i]->NeedGradient())
                    m_inputs[i]->RequestMatricesBeforeBackprop(matrixPool);
            }
        }

        //request matrices that are needed for gradient computation
        virtual void RequestMatricesBeforeBackprop(MatrixPool& matrixPool)
        {
            RequestMatrixFromPool(m_gradient, matrixPool);
        }

        //release gradient and temp matrices that no longer needed after all the children's gradients are computed.
        virtual void ReleaseMatricesAfterBackprop(MatrixPool& matrixPool)
        {
            if (!IsLeaf() && !RequiresPreCompute())
            {
            if (m_gradient != nullptr && m_gradient->GetMatrixType() != SPARSE) //since we don't have a sparse pool yet
                    ReleaseMatrixToPool(m_gradient, matrixPool);

                // Release the Value matrix only if the output value is needed during backprop
                // since in the case it isn't used, we release it during forward prop itself
                if (IsOutputNeededDuringBackprop() && m_value->GetMatrixType() != SPARSE && isValueSharable())
                    ReleaseMatrixToPool(m_value, matrixPool);
            }
        }

        virtual void DumpNodeInfo(const bool /*printValues*/, File& fstream) const;

        // TODO: similar to DumpInfo; used by ExperimentalNetworkBuilder test implementation
        /*HasToString::*/ wstring ToString() const
        {
            // we format it like "name : type rows x cols ( args )"
            wstring result = /*TidyName*/ (NodeName()) + L" : " + OperationName();
            result.append(msra::strfun::wstrprintf(L" [%s%s]", string(GetSampleLayout()).c_str(), HasMBLayout() ? " x *" : ""));
            if (m_inputs.empty())
                result.append(L" ()");
            else
            {
                wstring args;
                bool first = true;
                for (auto& child : m_inputs)
                {
                    if (first)
                        first = false;
                    else
                        args.append(L"\n");
                    args.append(/*TidyName*/ (child->NodeName()));
                }
                result += L" " + NestString(args, L'(', true, ')');
            }
            return result;
        }

        // update temporary variables of a node to match MBLayout
        virtual void UpdateFunctionMBSize() override { }

        void ValidateInferInputDimsFrom(const TensorShape & otherShape);

    public:
    static void MaskMissingColumnsToZero(Matrix<ElemType>& matrixToBeMasked, const MBLayoutPtr& pMBLayout, const FrameRange& fr)
        {
            //fprintf(stderr, "masking column range %d\n", (int)fr.timeIdxInSeq);
        MaskMissingColumnsTo(matrixToBeMasked, pMBLayout, fr, (ElemType) 0);
        }

    void /*ComputationNodeBase::*/ MaskMissingValueColumnsToZero(const FrameRange& fr) override final
        {
            //fprintf(stderr, "%ls %ls m_value ", NodeName().c_str(), OperationName().c_str());
            MaskMissingColumnsToZero(*m_value, m_pMBLayout, fr);
        }
    void /*ComputationNodeBase::*/ MaskMissingGradientColumnsToZero(const FrameRange& fr) override final
        {
            //fprintf(stderr, "%ls %ls m_gradient ", NodeName().c_str(), OperationName().c_str());
            MaskMissingColumnsToZero(*m_gradient, m_pMBLayout, fr);
        }

        // for debugging, set the gaps to NaN instead (to track whether it bubbles up somewhere)
    void InvalidateMissingValueColumns(const FrameRange& fr) override final
        {
            //fprintf(stderr, "invalidating %ls %ls m_value column range %d\n", NodeName().c_str(), OperationName().c_str(), (int)fr.timeIdxInSeq);
            MaskMissingColumnsTo(*m_value, m_pMBLayout, fr, Matrix<ElemType>::MakeNan(__LINE__));
        }
    void InvalidateMissingGradientColumns(const FrameRange& fr) override final
        {
            //fprintf(stderr, "invalidating %ls %ls m_gradient column range %d\n", NodeName().c_str(), OperationName().c_str(), (int)fr.timeIdxInSeq);
            MaskMissingColumnsTo(*m_gradient, m_pMBLayout, fr, Matrix<ElemType>::MakeNan(__LINE__));
        }

        // for debugging purposes
    void /*ComputationNodeBase::*/ PrintSelf(bool printMatrices = false) const
        {
            fprintf(stderr, "\n%ls[%s%s] = %ls", NodeName().c_str(), string(GetSampleLayout()).c_str(), HasMBLayout() ? " x *" : "", OperationName().c_str());

            if (!IsLeaf())
            {
                fprintf(stderr, "(");           
            for (size_t i = 0; i < GetNumInputs(); i++)
                {
                    if (i > 0)
                        fprintf(stderr, ", ");           
                    fprintf(stderr, "%ls[%s%s] = %ls", m_inputs[i] ? m_inputs[i]->NodeName().c_str() : L"NULL", string(m_inputs[i]->GetSampleLayout()).c_str(), m_inputs[i]->HasMBLayout() ? " x *" : "", OperationName().c_str());
                }
                fprintf(stderr, ")");           
            }

            if (printMatrices)
            {
            fprintf(stderr, "\n    $$$$ Function Values\n");
                Value().Print("FunctionValue");

            fprintf(stderr, "\n    $$$$ Gradient Values\n");
                Gradient().Print("GradientValue");
            }
        }

        // up-cast to make life easier
        static ComputationNodePtr UpCast(ComputationNodeBasePtr inode)
        {
            ComputationNodePtr node = dynamic_pointer_cast<ComputationNode<ElemType>>(inode);
            if (!node)
                InvalidArgument("an ComputationNodeBasePtr of mismatching precision was passed");
            return node;
        }

        inline ComputationNodePtr Input(const size_t inputIndex) const
        {
            if (inputIndex >= m_inputs.size())
            LogicError("Inputs: inputIndex %d is out of range for %ls %ls operation.", (int) inputIndex, NodeName().c_str(), OperationName().c_str());
            return UpCast(m_inputs[inputIndex]);
        }

    void /*ComputationNodeBase::*/ SetInput(const size_t childIndex, const ComputationNodeBasePtr& inode) override
        {
            const ComputationNodePtr node = UpCast(inode);

            //require first nodes specified before the second to avoid null nodes condition.
            if (childIndex > m_inputs.size())
                InvalidArgument("SetInput: You must specify the input for children with index less than this one first.");

            // expand the inputs to exist up to the desired index
            while (childIndex >= m_inputs.size())
                m_inputs.push_back(nullptr);

            // set the input value
            m_inputs[childIndex] = node;
        }

    const Matrix<ElemType>& Value() const
    {
        return *m_value;
    }
    Matrix<ElemType>& Value()
    {
        return *m_value;
    }

    const Matrix<ElemType>& Gradient() const
    {
        return *m_gradient;
    }
    Matrix<ElemType>& Gradient()
    {
        return *m_gradient;
    }
private:
    // map a tensor to a matrix
    // The leading dimension maps to rows, the rest to columns, for compat with sparse matrix lib.
    Matrix<ElemType> & TensorAsMatrix(Matrix<ElemType> & data)
    {
        size_t numRows = GetAsMatrixNumRows();
        size_t numCols = GetAsMatrixNumCols();
        // We only get here if the tensor indeed describes an 1D or 2D object. In that case, just verify the dimensions.
        data.VerifySize(numRows, numCols);
        return data;
    }
public:
    Matrix<ElemType> & ValueAsMatrix()
    {
        return TensorAsMatrix(*m_value);
    }
    Matrix<ElemType> & GradientAsMatrix()
    {
        return TensorAsMatrix(*m_gradient);
    }

    public:
#if 0   // only used for old implementation of PlusNode
        // Function to return the number of columns for whole batch or single frame
    size_t GetNumColsFor(const FrameRange& fr /*select frame or entire batch*/)
        {
            try
            {
                return ColumnRangeWithMBLayoutFor(Value().GetNumCols(), fr, m_pMBLayout).second;
            }
        catch (const logic_error& e) // catch the error and rethrow it with the node name attached
            {
                LogicError("%s, for %ls %ls operation.", e.what(), NodeName().c_str(), OperationName().c_str());
            }
        }
#endif

        // function to access any input and output, value and gradient, whole batch or single frame
        // Note: This returns a reference into 'data' in the form of a column slice, i.e. a small matrix object that just points into 'data'.
    Matrix<ElemType> DataFor(Matrix<ElemType>& data, const FrameRange& fr /*select frame or entire batch*/)
        {
            try
            {
                return DataWithMBLayoutFor(data, fr, m_pMBLayout);
            }
        catch (const logic_error& e) // catch the error and rethrow it with the node name attached
            {
                LogicError("%s, for %ls %ls operation.", e.what(), NodeName().c_str(), OperationName().c_str());
            }
        }

    Matrix<ElemType> ValueFor(const FrameRange& fr /*select frame or entire batch*/)
        {
            return DataFor(Value(), fr);
        }
    Matrix<ElemType> GradientFor(const FrameRange& fr /*select frame or entire batch*/)
        {
            return DataFor(Gradient(), fr);
        }
        // use the following two versions if you assume the inputs may contain gaps that must be set to zero because you want to reduce over frames with a BLAS operation
    Matrix<ElemType> MaskedValueFor(const FrameRange& fr /*select frame or entire batch*/)
        {
            MaskMissingValueColumnsToZero(fr);
            return ValueFor(fr);
        }
    Matrix<ElemType> MaskedGradientFor(const FrameRange& fr /*select frame or entire batch*/)
        {
            MaskMissingGradientColumnsToZero(fr);
            return GradientFor(fr);
        }
        // tensor version of the above functions
    TensorView<ElemType> DataTensorFor(Matrix<ElemType>& data, size_t rank, const FrameRange& fr)
        {
            try
            {
                return TensorView<ElemType>(data, GetTensorSliceFor(rank, fr));
            }
        catch (const logic_error& e) // catch the error and rethrow it with the node name attached
            {
                LogicError("%s, for %ls %ls operation.", e.what(), NodeName().c_str(), OperationName().c_str());
            }
        }
    TensorView<ElemType> ValueTensorFor(size_t rank, const FrameRange& fr)
        {
            return DataTensorFor(Value(), rank, fr);
        }
    TensorView<ElemType> GradientTensorFor(size_t rank, const FrameRange& fr)
        {
            return DataTensorFor(Gradient(), rank, fr);
        }

    private:

        // determine the size that we should set our Matrix storage to
        void DetermineDataSize(size_t & rows, size_t & cols) const
        {
            if (HasMBLayout())
            {
                rows = GetSampleMatrixNumRows();
                cols = GetSampleMatrixNumCols();
            }
            else
            {
                const auto & shape = GetSampleLayout();
                rows = shape.GetRank() > 0 ? shape[0] : 0;
                cols = rows > 0 ? shape.GetNumElements() / rows : 0;
            }
        }

    protected:

        // set the size of the underlying Matrix object to match node dimensions
        void UpdateDataSize(Matrix<ElemType>& m)
        {
            size_t rows, cols;
            DetermineDataSize(rows, cols);
            m.Resize(rows, cols);
        }
        // and verify the condition that UpdateDataSize() creates (used for sanity checking after loading parameters)
        void VerifyDataSize(Matrix<ElemType>& m)
        {
            size_t rows, cols;
            DetermineDataSize(rows, cols);
            m.VerifySize(rows, cols);
        }

    public:

        // update the actual matrix allocation for m_value based on the node dimension
        void UpdateFunctionValuesSize()
        {
            UpdateDataSize(Value());
        }

        // this is called before a node's ForwardProp() function is called (in loops: for the first time)
        // This is where we
        //  - update the node dimension based on actual MB size
        //  - (re-)allocate the m_value matrix, which may be shared across nodes and thus have changed dimensions
        virtual void /*IComputationNode::*/ BeginForwardProp() override // called before first iteration step of ForwardProp()
        {
            Base::BeginForwardProp();

            // update the actual m_value allocation
            if (!IsLeaf() && !RequiresPreCompute()) // TODO: guard this through overrides instead
                UpdateFunctionValuesSize();

            // give nodes a chance to update their internal state that may also have to match MB size
            UpdateFunctionMBSize();

            // and make sure dimensions are what we expect
            VerifyDataSize(Value());
        }

#ifdef _DEBUG
        // NaN checks
    virtual void /*IComputationNode::*/ EndForwardProp() override
        {
            Base::EndForwardProp();
#ifdef TRACK_GAP_NANS
        MaskMissingValueColumnsToZero(FrameRange(m_pMBLayout)); // HasNaN() operates on a whole matrix, so first flatten all gaps to 0
            if (Value().HasNan("EndForwardProp"))
                LogicError("%ls %ls operation unexpectedly produced NaN values.", NodeName().c_str(), OperationName().c_str());
#endif
#if 0
            MaskMissingValueColumnsToZero(FrameRange(m_pMBLayout)); // HasNaN() operates on a whole matrix, so first flatten all gaps to 0
            Value().Print(msra::strfun::utf8(NodeName()), 0, min(Value().GetNumRows()-1, 4), 0, min(Value().GetNumCols()-1, 4));
#endif
        InvalidateMissingValueColumns(FrameRange(m_pMBLayout)); // blast NaNs into columns that are gaps in a packed layout
        }
#endif

#if 0 // (keep it around in case we need to add stuff in the future)
        virtual void /*IComputationNode::*/BeginBackprop() override
        {
            Base::BeginBackprop();
        }
#endif

#ifdef _DEBUG
    virtual void /*IComputationNode::*/ EndBackprop() override
        {
            Base::EndBackprop();
#ifdef TRACK_GAP_NANS
            for (size_t i = 0; i < m_inputs.size(); i++)
            {
                ComputationNodePtr child = Input(i);
                if (child->m_needsGradient)
                {
                child->MaskMissingGradientColumnsToZero(FrameRange(child->GetMBLayout())); // HasNaN() operates on a whole matrix, so first flatten all gaps to 0
                    if (child->Gradient().HasNan("EndBackprop"))
                        LogicError("%ls %ls operation unexpectedly produced NaN gradients.", child->NodeName().c_str(), child->OperationName().c_str());
                }
            }
#endif
        }
#endif

        // this is the entry point from Network; while it will call virtual BackpropTo() into the actual node implementation
        // TODO: move to -Base (or -Network?)
    void Backprop(const FrameRange& fr, bool childrenInThisLoop, bool childrenInOuterLoop) override
        {
            if (fr.IsAllFrames() && IsPartOfLoop() && childrenInThisLoop)
                LogicError("%ls %ls operation: Backprop called with whole-batch FrameRange on node that participates in a loop", NodeName().c_str(), OperationName().c_str());

            for (size_t i = 0; i < m_inputs.size(); i++)
            {
                ComputationNodePtr child = Input(i);
                if (child->m_needsGradient &&
                (childrenInThisLoop && child->IsPartOfLoop() == IsPartOfLoop() ||
                 childrenInOuterLoop && child->IsPartOfLoop() != IsPartOfLoop()))
                {
                    //fprintf(stderr, "Backprop: %ls %ls operation -> child %d %ls %ls\n", NodeName().c_str(), OperationName().c_str(), (int)i, child->NodeName().c_str(), child->OperationName().c_str());
                    if (!m_needsGradient)
                        LogicError("%ls %ls operation has m_needsGradient set to false but children require it.", NodeName().c_str(), OperationName().c_str());
#ifdef DISPLAY_DEBUG
                fprintf(stderr, "    [%lu]: %ls(%ls)\n", i, child->OperationName().c_str(), child->NodeName().c_str());
#endif
#if DUMPOUTPUT
                    fprintf(stderr, "Backprop%d_%ls\n", i, NodeName().c_str());
#endif
                child->LazyZeroGradient(); // set gradient to 0 if this is the first time

                    // If we propagate from a loop to a node that is outside the loop, we are not efficient.
                    // This case is handled by SEQTraversalFlowControlNode::Backprop().
                    // The check below is to verify that.
                    if (IsPartOfLoop() && !child->IsPartOfLoop() && !fr.IsAllFrames())
                    {
                        LogicError("Backprop: Inefficiency: %ls %ls operation in loop propagates gradient to non-loop %ls %ls\n",
                                   NodeName().c_str(), OperationName().c_str(), child->NodeName().c_str(), child->OperationName().c_str());
                    }

                    //fprintf(stderr, "BackpropTo %d %d %ls %ls\n", (int)fr.timeIdxInSeq, (int)i, NodeName().c_str(), OperationName().c_str());
                BackpropTo(i, fr); // this computes partial wrt to the child and sums the gradient value in the child
                }
#ifdef DISPLAY_DEBUG
            else
                fprintf(stderr, "    [%lu]: %s(%s) (no gradient needed so don't compute for)\n", i, child->OperationName().c_str(), child->NodeName().c_str());
#endif
            }
        }

        // TODO: why of the inputs, and not the node itself?
    void /*ComputationNodeBase::*/ ZeroGradientsOfInputs() override // clears the lazy-init flags (LazyZeroGradient() actually clears the values lazily)
        {
            for (size_t i = 0; i < m_inputs.size(); i++)
                Input(i)->m_gradientInitialized = false;
        }

        // lazy resetting of gradient
        void LazyZeroGradient()
        {
            if (!m_needsGradient)
                LogicError("%ls %ls operation: LazyZeroGradient() called although this node needs no gradient.", NodeName().c_str(), OperationName().c_str());

            if (m_gradientInitialized)
                return;

            UpdateDataSize(Gradient());
            Gradient().SetValue(0);

            m_gradientInitialized = true;
        }

        // NOTE: we should reimplement this to be thread-safe and use a larger than requested initialized memory block
        // we can then just wrap that memory block in a matrix of the correct dimensions since it will be const no one can change it
        // should only need one memory block per device
        // Thread-safety could be achieved by changing this to a shared_ptr.
        // When using the TensorView interface, one could instead just use a 1x1 matrix with a view that broadcasts its columns (stride 0).
        static const Matrix<ElemType>& ConstOnes(const size_t rows, const size_t cols, const DEVICEID_TYPE deviceId)
        {
            if (s_constOnes.find(rows) == s_constOnes.end() ||
                s_constOnes[rows].find(cols) == s_constOnes[rows].end()) //not found
            {
            Matrix<ElemType>* matrix = new Matrix<ElemType>(rows, cols, (DEVICEID_TYPE) deviceId);
                matrix->SetValue(1);
                s_constOnes[rows][cols] = matrix;
            }

            Matrix<ElemType>* m = s_constOnes[rows][cols];
            m->TransferFromDeviceToDevice(m->GetDeviceId(), deviceId);

            return *m;
        }

        void CreateGradientMatrixIfNull()
        {
            CreateMatrixIfNull(m_gradient);
        }

        void MarkValueNonSharable() override
        {
            m_valueSharable = false; 
            CreateMatrixIfNull(m_value);
        }

    protected:
        // this function is used to create matrices for those needed before matrix pool is available
        // e.g., for model parameters and input nodes you will need to resize the functions based on NDL
        // and before matrix pool is available
        void CreateMatrixIfNull(shared_ptr<Matrix<ElemType>>& matrixPtr)
        {
            if (!matrixPtr)
                matrixPtr = make_shared<Matrix<ElemType>>(m_deviceId);
        }

        void RequestMatrixFromPool(shared_ptr<Matrix<ElemType>>& matrixPtr, MatrixPool& matrixPool)
        {
            if (matrixPtr == nullptr)
            {
                matrixPtr = matrixPool.Request<ElemType>(m_deviceId);
            }
        }

        void ReleaseMatrixToPool(shared_ptr<Matrix<ElemType>>& matrixPtr, MatrixPool& matrixPool)
        {
            assert(matrixPtr != nullptr);
            matrixPool.Release<ElemType>(matrixPtr);
        }

        // print node values
        void PrintNodeValuesToFile(const bool printValues, File& fstream) const
        {
            if (printValues)
            {
                fstream << wstring(L"\n");
            const Matrix<ElemType>& m = Value();
            for (size_t i = 0; i < m.GetNumRows(); i++)
                {
                for (size_t j = 0; j < m.GetNumCols(); j++)
                    {
                    fstream << m(i, j);
                    }
                    fstream << wstring(L"\n");
                }
                fstream << wstring(L"####################################################################");
            }
        }

    public:
        virtual void CopyTo(ComputationNodeBasePtr nodeP, const std::wstring& newName, const CopyNodeFlags flags) const override
        {
            Base::CopyTo(nodeP, newName, flags);
            if (flags & CopyNodeFlags::copyNodeValue)
            {
                auto node = UpCast(nodeP);
                *node->m_value = *m_value;
                if (m_gradient)
                    *node->m_gradient = *m_gradient;
                else
                    node->m_gradient = nullptr;
            }
        }

        // duplicate a node
        ComputationNodeBasePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags)
        {
            const std::wstring& name = (newName == L"") ? NodeName() : newName;
            ComputationNodeBasePtr node(NewThis(m_deviceId, name)); // NewThis() is a virtual function that creates a new node of the actual type of 'this'
            node->CopyTo(shared_from_this(), newName, flags);       // note: shared_from_this() is the base class, but CopyTo() up-casts it as needed
            return node;
        }

        // these are used to export hidden state activations
    virtual bool GetHistory(Matrix<ElemType>&, bool)
    {
        return false;
    }
    virtual void SetHistory(const Matrix<ElemType>&)
    {
    }

        /// these two are used to pass gradients from future minibatch
    virtual void GetErrorsToPreviousMinibatch(Matrix<ElemType>&)
    {
    }
    virtual void SetErrorsFromFutureMinibatch(Matrix<ElemType>&)
    {
    }

    protected:
        shared_ptr<Matrix<ElemType>> m_value, m_gradient;

        static std::map<size_t, std::map<size_t, Matrix<ElemType>*>> s_constOnes;
    };

    // convenience wrapper for ComputationNode::New()
template <class C, class... _Types>
inline shared_ptr<C> New(_Types&&... _Args)
    {
        return make_shared<C>(forward<_Types>(_Args)...);
    }

    // =======================================================================
    // ComputationNodeNonLooping -- abstract base class for computation nodes that do not implement eval/partial for individual frames
    // Such as CRFNode, LSTMNode, ParallelNode, SequenceDecoderNode, TimeReverseNode (BatchModeNode), and TransposeNode.
    // =======================================================================

    // This will provide default implementations for those two functions that will fail at runtime with a meaningful error.
    // TODO: Most of these are reduce nodes that output a single number, no MBLayout. Maybe abstract those out further
template <class ElemType>
    class ComputationNodeNonLooping : public ComputationNode<ElemType>
    {
        typedef ComputationNode<ElemType> Base;

    public:
    ComputationNodeNonLooping(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name)
    {
    }

        // these two implement the ComputationNode<> interface
    void ForwardProp(const FrameRange& fr) override final
        {
            if (fr.IsAllFrames())
                ForwardPropNonLooping();
            else
                LogicError("%s node should never be in a loop.", typeid(*this).name());
        }
    void BackpropTo(const size_t inputIndex, const FrameRange& fr) override final
        {
            if (fr.IsAllFrames())
                BackpropToNonLooping(inputIndex);
            else
                LogicError("%s node should never be in a loop.", typeid(*this).name());
        }

        // non-looping node types instead implement these functions
        virtual void ForwardPropNonLooping() = 0;
        virtual void BackpropToNonLooping(size_t inputIndex) = 0;
    };

    // =======================================================================
    // FlowControlNode -- special wrapper node for use by ComputationNetwork only
    // =======================================================================

    class FlowControlNode : public ComputationNodeBase
    {
        typedef ComputationNodeBase Base;

    public:
    FlowControlNode()
        : ComputationNodeBase(DEVICEID_NOTYETDETERMINED /*we don't own matrices*/, L"" /*name: we don't care*/)
    {
    }

#pragma warning(disable : 4100)
        // these are meant to be implemented by ComputationNode<ElemType> but should never be called on traversal nodes
        // TODO: There are too many of these. This indicates improper class hierarchies.
    virtual ComputationNodeBase* NewThis(DEVICEID_TYPE deviceId, const wstring& name) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void Validate(bool isFinalValidationPass) override
    {
        NOT_IMPLEMENTED;
    } // main base validation function
    virtual void Save(File& fstream) const override
    {
        NOT_IMPLEMENTED;
    }
    virtual void Load(File& /*fstream*/, size_t /*modelVersion*/) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void CopyTo(ComputationNodeBasePtr node, const std::wstring& newName, const CopyNodeFlags flags) const override
    {
        NOT_IMPLEMENTED;
    }
    virtual ComputationNodeBasePtr Duplicate(const std::wstring& newName, const CopyNodeFlags flags) override
    {
        NOT_IMPLEMENTED;
    }
    virtual double Get00Element() const override
    {
        NOT_IMPLEMENTED;
    }
    virtual void UpdateFunctionMBSize() override
    {
        NOT_IMPLEMENTED;
    }
    virtual void AttachInputs(const std::vector<ComputationNodeBasePtr>& inputs) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void PrintSelf(bool) const override
    {
        NOT_IMPLEMENTED;
    }
    virtual void ValidateInferInputDimsFrom(const TensorShape &) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void SetInput(const size_t, const Microsoft::MSR::CNTK::ComputationNodeBase::ComputationNodeBasePtr&) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void ZeroGradientsOfInputs(void) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void MaskMissingValueColumnsToZero(const Microsoft::MSR::CNTK::FrameRange&) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void MaskMissingGradientColumnsToZero(const Microsoft::MSR::CNTK::FrameRange&) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void InvalidateMissingValueColumns(const Microsoft::MSR::CNTK::FrameRange&) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void InvalidateMissingGradientColumns(const Microsoft::MSR::CNTK::FrameRange&) override
    {
        NOT_IMPLEMENTED;
    }
    virtual void NotifyFunctionValuesMBSizeModified(void) override { NOT_IMPLEMENTED; }
    virtual std::wstring ToString(void) const override
    {
        NOT_IMPLEMENTED;
    }
        // these are meant to be called during computation, so provide dummy implementations
    virtual bool RequiresPreCompute() const override
    {
        return false;
    } // return true if the node's value should be computed before the normal training. e.g., mean and invStd of input features.
    virtual void PrintSelfBeforeValidation() const override
    {
    }
    virtual void DumpNodeInfo(const bool /*printValues*/, File& fstream) const override
    {
    }

    protected:
public:                                                // needed in ComputationNetwork::FindInRecurrentLoops(), which really should be part of SEQTraversalFlowControlNode
    std::vector<ComputationNodeBasePtr> m_nestedNodes; // nodes tucked away in this node, in evaluation order
    };

    // =======================================================================
    // ILateAttachingNode -- helper wrapper class for ComputationNodes that must AttachInputs() late due to circular references
    // =======================================================================

    // Instantiate with LateAttachingNode<node type>(lambda, args for node constructor).
    // To resolve, call AttachInputs()
    // TODO: This is a bit indirect. Can it be done more nicely?
struct ILateAttachingNode
{
    virtual void LateAttachInputs() = 0;
};
template <class N>
    class LateAttachingNode : public N, public ILateAttachingNode
    {
        typedef typename N::OurElemType ElemType;
        function<void(ComputationNode<ElemType>*)> attachInputs;

    public:
        // constructor
    template <class... _Types>
    LateAttachingNode(DEVICEID_TYPE deviceId, const wstring& name, const function<void(ComputationNode<ElemType>*)>& attachInputs, _Types&&... _Args)
        : attachInputs(attachInputs), N(deviceId, name, forward<_Types>(_Args)...)
    {
    }
        // the one member that does the work
    void /*ILateAttachingNode::*/ LateAttachInputs()
        {
            attachInputs(dynamic_cast<N*>(this));
        attachInputs = [](ComputationNode<ElemType>*)
        {
            LogicError("LateAttachingNode::AttachInputs: must only be called once");
        };
        }
    };

    // =======================================================================
    // IRecurrentNode -- helper wrapper class for ComputationNodes that can be recurrent
    // =======================================================================

struct IRecurrentNode
{
    virtual int GetRecurrenceSteppingDirection() const = 0;
};

// =======================================================================
// helper macro to ease access to base members in presence of C++ two-phase name lookup
// =======================================================================

// Add 'typedef ComputationNode<ElemType> Base; UsingComputationNodeMembersBoilerplate;' at the start of each derived class
// (some derived classes define a similar macro; there please modify the typedef for Base accordingly.)
// This macro imports, one by one, every member of ComputationNode into the name space of the derived class.
// Without this, one would have to use the name prefix, or alternatively this->, in front of all base member,
// because the standard does not allow the compiler to do that for you (as MSVC still kindly does).
// If you add new members to ComputationNode, please also add them here.
// This macro expects 'Base' to be the name of the base class. Please also use 'Base' outside this macro to make it less likely to accidentally call the wrong base class members.
// Note: Whoever invented that C++ insanity called two-phase name lookup shall rot in hell, for the crime of causing infinite pain on unsuspecting programmers. [fseide]
#define UsingComputationNodeMembers /*without OperationName; needed to support inconsistent pattern of InputValue--TODO: This comment it out of date. */ \
    \
protected:                                                                                                                                               \
    typedef shared_ptr<ComputationNode<ElemType>> ComputationNodePtr;                                                                                    \
    using Base::m_deviceId;                                                                                                                              \
    using Base::shared_from_this;                                                                                                                        \
    using Base::GetDeviceId;                                                                                                                             \
    using Base::SetDims;                                                                                                                                 \
    using Base::SetDims1;                                                                                                                                \
    using Base::GetSampleMatrixNumRows;                                                                                                                              \
    using Base::GetSampleMatrixNumCols;                                                                                                                              \
    using Base::GetAsMatrixNumRows;                                                                                                                              \
    using Base::GetAsMatrixNumCols;                                                                                                                              \
    using Base::GetTensorShape;                                                                                                                          \
    using Base::UpdateFunctionValuesSize;                                                                                                                \
    using Base::LoadValue;                                                                                                                               \
    using Base::m_pMBLayout;                                                                                                                             \
    using Base::GetNumTimeSteps;                                                                                                                         \
    using Base::GetNumParallelSequences;                                                                                                                 \
    using Base::MaskMissingColumnsToZero;                                                                                                                \
    using Base::MaskMissingValueColumnsToZero;                                                                                                           \
    using Base::MaskMissingGradientColumnsToZero;                                                                                                        \
    using Base::InvalidateMissingValueColumns;                                                                                                           \
    using Base::InvalidateMissingGradientColumns;                                                                                                        \
    using Base::DataFor;                                                                                                                                 \
    using Base::ValueFor;                                                                                                                                \
    using Base::GradientAsMatrix; using Base::Gradient;                                                                                                                                \
    using Base::GradientFor;                                                                                                                             \
    using Base::MaskedValueFor;                                                                                                                          \
    using Base::MaskedGradientFor;                                                                                                                       \
    using Base::DataTensorFor;                                                                                                                           \
    using Base::ValueTensorFor;                                                                                                                          \
    using Base::GradientTensorFor;                                                                                                                       \
    using Base::ForwardProp;                                                                                                                             \
    using Base::BackpropTo;                                                                                                                              \
    using Base::m_inputs;                                                                                                                                \
    using Base::m_value;                                                                                                                                 \
    using Base::m_gradient;                                                                                                                              \
    using Base::m_sampleLayout;                                                                                                                          \
    using Base::m_parameterUpdateRequired;                                                                                                               \
    using Base::m_nodeName;                                                                                                                              \
    using Base::CreateMatrixIfNull;                                                                                                                      \
    using Base::RequestMatrixFromPool;                                                                                                                   \
    using Base::ReleaseMatrixToPool;                                                                                                                     \
    using Base::CreateUniqId;                                                                                                                            \
    using Base::GetNumInputs;                                                                                                                            \
    using Base::ZeroGradientsOfInputs;                                                                                                                   \
    using Base::VerifyDims; using Base::VerifyDataSize;                                                                                                                              \
    using Base::ConstOnes;                                                                                                                               \
    using Base::DetermineElementwiseTensorRank;                                                                                                          \
    using Base::GetSampleLayout;                                                                                                                         \
    using Base::GetInputSampleLayout;                                                                                                                    \
    using Base::InferMBLayoutFromInputsForStandardCase;                                                                                                  \
    using Base::CopyTo;                                                                                                                                  \
    using Base::CreateUniqNodeName;                                                                                                                      \
    using Base::DetachInputs;                                                                                                                            \
    using Base::GetInputsFromConfig;                                                                                                                     \
    using Base::DumpNodeInfo;                                                                                                                            \
    using Base::EnumerateNodes;                                                                                                                          \
    using Base::HasMBLayout;                                                                                                                             \
    using Base::GetMBLayout;                                                                                                                             \
    using Base::LinkToMBLayout;                                                                                                                          \
    using Base::Input;                                                                                                                                   \
    using Base::SetInput;                                                                                                                                \
    using Base::IsEqualTo;                                                                                                                               \
    using Base::IsOutputOlderThanInputs;                                                                                                                 \
    using Base::IsLeaf;                                                                                                                                  \
    using Base::SetParameterUpdateRequired;                                                                                                              \
    using Base::Load;                                                                                                                                    \
    using Base::PrintNodeValuesToFile;                                                                                                                   \
    using Base::PrintSelfBeforeValidation;                                                                                                               \
    using Base::Save;                                                                                                                                    \
    using Base::UpdateFunctionMBSize;                                                                                                                    \
    using Base::RequestMatricesBeforeForwardProp;                                                                                                        \
    using Base::ReleaseMatricesAfterForwardProp;                                                                                                         \
    using Base::RequestMatricesBeforeBackprop;                                                                                                           \
    using Base::ReleaseMatricesAfterBackprop;                                                                                                            \
    using Base::InputUsedInComputingInputNodesGradients;                                                                                                 \
    using Base::OutputUsedInComputingInputNodesGradients; using Base::m_valueSharable;                                                                                              \
    using Base::Validate;                                                                                                                                \
    using Base::ValidateUnaryMap;                                                                                                                        \
    using Base::ValidateBinaryZip;                                                                                                                       \
    using Base::ValidateUnaryReduce;                                                                                                                     \
    using Base::ValidateBinaryReduce;                                                                                                                    \
    using Base::ValidateInferBinaryInputDims;                                                                                                            \
    using Base::ValidateInferInputDimsFrom;                                                                                                                  \
    \
public:                                                                                                                                                  \
    using Base::RequiresPreCompute;                                                                                                                      \
    using Base::AttachInputs;                                                                                                                            \
    using Base::CreateGradientMatrixIfNull;                                                                                                              \
    using Base::NodeName;                                                                                                                                \
    using Base::ValueAsMatrix; using Base::Value;

#define ComputationNodeBoilerplate                                                             \
    \
protected: /* some boilerplate goes here */                                                    \
    virtual const std::wstring OperationName() const override                                  \
    {                                                                                          \
        return TypeName();                                                                     \
    }                                                                                          \
    virtual ComputationNodeBase* NewThis(DEVICEID_TYPE deviceId, const wstring& name) override \
    {                                                                                          \
        return new typename std::remove_reference<decltype(*this)>::type(deviceId, name);      \
    }

#define UsingComputationNodeMembersBoilerplate \
    ComputationNodeBoilerplate;                \
    UsingComputationNodeMembers

    // =======================================================================
    // a few standard base classes for N-nary operations
    // =======================================================================

    // -----------------------------------------------------------------------
    // UnaryElementWiseNode (operand)
    //
    // unary elementwise operations that are implemented with the tensor lib
    //
    // Derived clases only need to override ForwardProp() and BackpropTo().
    // -----------------------------------------------------------------------

template <class ElemType>
    class UnaryElementWiseNode : public ComputationNode<ElemType>, public NumInputs<1>
    {
    typedef ComputationNode<ElemType> Base;
    UsingComputationNodeMembers;

    public:
    UnaryElementWiseNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name)
    {
    }

    virtual void /*ComputationNodeBase::*/ Validate(bool isFinalValidationPass) override
        {
            ValidateUnaryMap(isFinalValidationPass);
        }
    };

#define UsingUnaryElementwiseNodeBaseMembers UsingComputationNodeMembersBoilerplate;

    // -----------------------------------------------------------------------
    // BinaryElementWiseNode (operand1, operand2)
    //
    // binary elementwise operations that are implemented with the tensor lib
    //
    // Derived clases only need to override ForwardProp() and BackpropTo().
    // -----------------------------------------------------------------------

template <class ElemType>
    class BinaryElementWiseNode : public ComputationNode<ElemType>, public NumInputs<2>
    {
    typedef ComputationNode<ElemType> Base;
    UsingComputationNodeMembers;

    public:
    BinaryElementWiseNode(DEVICEID_TYPE deviceId, const wstring& name)
        : Base(deviceId, name)
    {
    }

        virtual bool OutputUsedInComputingInputNodesGradients() const override
        {
#if DUMPOUTPUT
            return true;
#else
            // By default, the BinaryElementWiseNode does not require its output value for computing
            // the gradients of its input nodes
            return false;
#endif
        }

        // By default, the BinaryElementWiseNode does not require any of it's input's values for computing
        // the gradients of its input nodes
    virtual bool InputUsedInComputingInputNodesGradients(size_t /*childIndex*/) const override
    {
        return false;
    }

    virtual void /*IComputationNode::*/ BeginForwardProp() override // called before first iteration step of ForwardProp()
        {
            Base::BeginForwardProp();
            // we switch result to dense as a work-around because ColumnSlice doesn't support all the sparse formats
            // TODO: This is a stopgap. Is this the right thing to do? It changes the matrix type in-place.
            Value().SwitchToMatrixType(MatrixType::DENSE, MatrixFormat::matrixFormatDense, false);
        }

    virtual void /*ComputationNodeBase::*/ Validate(bool isFinalValidationPass) override
        {
        ValidateBinaryZip(isFinalValidationPass, true /*allowMultiples*/);
        }
    };

#define UsingBinaryElementwiseNodeBaseMembers UsingComputationNodeMembersBoilerplate;

#pragma endregion base computation class
} } }

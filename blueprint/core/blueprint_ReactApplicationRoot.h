/*
  ==============================================================================

    blueprint_ReactApplicationRoot.h
    Created: 9 Dec 2018 10:20:37am

  ==============================================================================
*/

#pragma once

#include <map>

#include "blueprint_ImageView.h"
#include "blueprint_RawTextView.h"
#include "blueprint_ScrollView.h"
#include "blueprint_ScrollViewContentShadowView.h"
#include "blueprint_ShadowView.h"
#include "blueprint_TextShadowView.h"
#include "blueprint_TextView.h"
#include "blueprint_View.h"


namespace blueprint
{

    /** This struct defines the set of functions that form the native interface
        for the JavaScript evaluation context.
     */
    struct BlueprintNative
    {
        static duk_ret_t createViewInstance (duk_context *ctx);
        static duk_ret_t createTextViewInstance (duk_context *ctx);
        static duk_ret_t setViewProperty (duk_context *ctx);
        static duk_ret_t setRawTextValue (duk_context *ctx);
        static duk_ret_t addChild (duk_context *ctx);
        static duk_ret_t removeChild (duk_context *ctx);
        static duk_ret_t getRootInstanceId (duk_context *ctx);
    };

    /** Allocates a new Duktape heap and initializes the BlueprintNative API therein. */
    duk_context* initializeDuktapeContext();

    //==============================================================================
    /** The ReactApplicationRoot class prepares and maintains a Duktape evaluation
        context with the relevant hooks for supporting the Blueprint render
        backend.
     */
    class ReactApplicationRoot : public View, public juce::Timer
    {
    public:
        //==============================================================================
        // We allow registering arbitrary view types with the React context by way of
        // a "ViewFactory" here which is a user-defined function that produces a View
        // and a corresponding ShadowView.
        typedef std::pair<std::unique_ptr<View>, std::unique_ptr<ShadowView>> ViewPair;
        typedef std::function<ViewPair()> ViewFactory;

        //==============================================================================
        ReactApplicationRoot()
        {
            jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

            // Create a duktape context
            ctx = initializeDuktapeContext();

            // Push a pointer to this root instance
            duk_push_global_stash(ctx);
            duk_push_pointer(ctx, (void *) this);
            duk_put_prop_string(ctx, -2, "rootInstance");

            // Assign our root level shadow view
            _shadowView = std::make_unique<ShadowView>(this);

            // And install view types
            installNativeViewTypes();
        }

        ~ReactApplicationRoot()
        {
            stopTimer();
            duk_destroy_heap(ctx);
        }

        //==============================================================================
        /** Override the default View behavior. */
        void resized() override
        {
            performShadowTreeLayout();
        }

        /** Implement the timer callback; only to be initiated after the bundle has
            been evaluated.
         */
        void timerCallback() override
        {
            jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());

            // Push the schedulerInterrupt function to the top of the stack and call it.
            duk_get_global_string(ctx, "__schedulerInterrupt__");
            [[maybe_unused]] const duk_int_t rc = duk_pcall(ctx, 0);

            if (rc != DUK_EXEC_SUCCESS)
                DBG("Duktape scheduler interrupt error: " << duk_safe_to_string(ctx, -1));

            duk_pop(ctx);
        }

        //==============================================================================
        /** Reads a JavaScript bundle from file and evaluates it in the Duktape context. */
        void evalScript (const juce::String& script)
        {
            duk_push_string(ctx, script.toRawUTF8());

            if (duk_peval(ctx) != 0) {
                printf("Script evaluation failed: %s\n", duk_safe_to_string(ctx, -1));
            }

            duk_pop(ctx);

            // Schedule the timer...
            startTimer(4);
        }

        /** Enables keyboard focus on this component, expecting keypress events to reload
            the javascript bundle.
         */
        void enableHotkeyReloading()
        {
            setWantsKeyboardFocus(true);
        }

        /** Rebuilds a new Duktape context, reads and executes the sourceFile. */
        bool keyPressed (const juce::KeyPress& key) override
        {
            bool cmd = key.getModifiers().isCommandDown();
            auto r = key.isKeyCode(82);

            if (cmd && r)
            {
                duk_destroy_heap(ctx);
                removeAllChildren();
                viewTable.clear();
                shadowViewTable.clear();
                ctx = initializeDuktapeContext();
                _shadowView = std::make_unique<ShadowView>(this);
                // TODO: Disabling this for now; need to rethink the
                // interface and whether or not this kind of functionality
                // belongs within the ReactApplicationRoot class.
                // runScript(sourceFile);
            }

            return true;
        }

        /** A simple accessor for the underlying Duktape context. */
        duk_context* getDuktapeContext()
        {
            return ctx;
        }

        //==============================================================================
        // VIEW MANAGER STUFF: SPLIT OUT?

        /** Registers a new dynamic view type and its associated factory. */
        void registerViewType(const juce::String& typeId, ViewFactory f)
        {
            // If you hit this jassert, you're trying to register a type which
            // has already been registered!
            jassert (viewFactories.find(typeId) == viewFactories.end());
            viewFactories[typeId] = f;
        }

        /** Creates a new view instance and registers it with the view table. */
        ViewId createViewInstance(const juce::String& viewType)
        {
            // We can't create a view instance of a type that hasn't been registered.
            jassert (viewFactories.find(viewType) != viewFactories.end());

            auto [view, shadowView] = viewFactories[viewType]();
            ViewId vid = view->getViewId();

            viewTable[vid] = std::move(view);
            shadowViewTable[vid] = std::move(shadowView);

            return vid;
        }

        /** Creates a new text view instance and registers it with the view table. */
        ViewId createTextViewInstance(const juce::String& value)
        {
            std::unique_ptr<View> view = std::make_unique<RawTextView>(value);
            ViewId id = view->getViewId();

            viewTable[id] = std::move(view);
            return id;
        }

        void setViewProperty (ViewId viewId, const juce::Identifier& name, const juce::var& value)
        {
            const auto& [view, shadow] = getViewHandle(viewId);

            view->setProperty(name, value);
            shadow->setProperty(name, value);

            // For now, we just assume that any new property update means we
            // need to redraw or lay out our tree again. This is an easy future
            // optimization.
            performShadowTreeLayout();
            view->repaint();
        }

        void setRawTextValue (ViewId viewId, const juce::String& value)
        {
            View* view = getViewHandle(viewId).first;

            if (auto* rawTextView = dynamic_cast<RawTextView*>(view))
            {
                // Update text
                rawTextView->setText(value);

                if (auto* parent = dynamic_cast<TextView*>(rawTextView->getParentComponent()))
                {
                    // If we have a parent already, find the parent's shadow node and
                    // mark it dirty, then we'll issue a new layout call
                    ShadowView* parentShadowView = getViewHandle(parent->getViewId()).second;

                    if (auto* textShadowView = dynamic_cast<TextShadowView*>(parentShadowView))
                    {
                        textShadowView->markDirty();
                        performShadowTreeLayout();
                    }

                    // Then we need to paint, but the RawTextView has no idea how to paint its text,
                    // we need to tell the parent to repaint its children.
                    parent->repaint();
                }
            }
        }

        void addChild (ViewId parentId, ViewId childId, int index = -1)
        {
            const auto& [parentView, parentShadowView] = getViewHandle(parentId);
            const auto& [childView, childShadowView] = getViewHandle(childId);

            if (auto* textView = dynamic_cast<TextView*>(parentView))
            {
                // If we're trying to append a child to a text view, it will be raw text
                // with no accompanying shadow view, and we'll need to mark the parent
                // TextShadowView dirty before the subsequent layout pass.
                jassert (dynamic_cast<RawTextView*>(childView) != nullptr);
                jassert (childShadowView == nullptr);

                parentView->addChild(childView, index);
                dynamic_cast<TextShadowView*>(parentShadowView)->markDirty();
            }
            else
            {
                parentView->addChild(childView, index);
                parentShadowView->addChild(childShadowView, index);
            }

            performShadowTreeLayout();
        }

        void removeChild (ViewId parentId, ViewId childId)
        {
            const auto& [parentView, parentShadowView] = getViewHandle(parentId);
            const auto& [childView, childShadowView] = getViewHandle(childId);

            // TODO: Set a View::removeChild method and call into that here. Make
            // that method virtual so that, e.g., the scroll view can override to
            // remove the child from its viewport
            parentView->removeChildComponent(childView);

            // Here we have to clear the view table of all children of this view.
            // React may clear a whole subtree from the interface by removing a
            // single component at the root of the tree. Because the view table
            // is a flat map of viewId to View, if we only remove that root view
            // from the table we leave all of its children dangling, which confuses
            // subsequent functionality like `getViewHandle` or `getViewByRefId`
            std::vector<ViewId> childIds;
            enumerateChildViewIds(childIds, childView);

            for (auto& id : childIds)
                viewTable.erase(id);

            // We might be dealing with a text view, in which case we expect a null
            // shadow view.
            if (parentShadowView && childShadowView)
            {
                parentShadowView->removeChild(childShadowView);

                // Then here, since we now know we have a child shadow view,
                // we try also to remove its children from the shadowViewTable to
                // prevent dangling children like in the viewTable above.
                for (auto& id : childIds)
                    shadowViewTable.erase(id);
            }

            performShadowTreeLayout();
        }

        void enumerateChildViewIds (std::vector<ViewId>& ids, View* v)
        {
            for (auto* child : v->getChildren())
            {
                // Some view elements may mount a plain juce::Component, such as the
                // ScrollView mounting a juce::Viewport which is a juce::Component but
                // not a juce::View. Such elements aren't in our table and can be skipped
                if (auto* childView = dynamic_cast<View*>(child))
                {
                    enumerateChildViewIds(ids, childView);
                }
            }

            ids.push_back(v->getViewId());
        }

        /** Returns a pointer pair to the view associated to the given id. */
        std::pair<View*, ShadowView*> getViewHandle (ViewId viewId)
        {
            if (viewId == getViewId())
                return {this, _shadowView.get()};

            if (viewTable.find(viewId) != viewTable.end())
                return {viewTable[viewId].get(), shadowViewTable[viewId].get()};

            // If we land here, you asked for a view that we don't have.
            jassertfalse;
            return {nullptr, nullptr};
        }

        /** Walks the view table, returning the first view with a `refId`
         *  whose value equals the provided id.
         */
        View* getViewByRefId (const juce::Identifier& refId)
        {
            if (refId == getRefId())
                return this;

            for (auto& pair : viewTable)
            {
                auto* view = pair.second.get();

                if (refId == view->getRefId())
                    return view;
            }

            return nullptr;
        }

        /** Register a native method to be called from the script engine. */
        void registerNativeMethod(const std::string& name, std::function<void(const juce::var::NativeFunctionArgs&)> fn) {
            // Push the function into the registry and hang onto its index
            size_t fnIndex = methodRegistry.size();
            methodRegistry.push_back(fn);

            // Pull __BlueprintNative__ onto the stack
            duk_push_global_object(ctx);
            duk_get_prop_string(ctx, -1, "__BlueprintNative__");
            duk_require_object(ctx, -1);

            // Push a lightfunc that can retrieve the registry index via its magic.
            // We want the registered method to be able to capture and carry a closure,
            // but those functions can't be converted to a standard c function pointer. We
            // therefore hold those functions in a local registry and push a wrapper function
            // into the script engine, where the wrapper knows which registry index to call back
            // to via duktape's lightfunc "magic" feature.
            duk_push_c_lightfunc(ctx, [](duk_context* ctx) -> duk_ret_t {
                // Retrieve the root instance pointer
                duk_push_global_stash(ctx);
                duk_get_prop_string(ctx, -1, "rootInstance");
                ReactApplicationRoot* root = reinterpret_cast<ReactApplicationRoot*>(duk_get_pointer(ctx, -1));
                duk_pop_2(ctx);

                jassert (root != nullptr);

                unsigned int fnIndex = ((unsigned int) duk_get_current_magic(ctx)) & 0xffffU;
                std::vector<juce::var> args;

                // Build up the arguments vector
                int nargs = duk_get_top(ctx);

                for (int i = 0; i < nargs; ++i)
                {
                    switch (duk_get_type(ctx, i))
                    {
                        case DUK_TYPE_STRING:
                            args.emplace_back(duk_get_string(ctx, i));
                            break;
                        case DUK_TYPE_NUMBER:
                            args.emplace_back(duk_get_number(ctx, i));
                            break;
                        case DUK_TYPE_BOOLEAN:
                            args.emplace_back((bool) duk_get_boolean(ctx, i));
                            break;
                        default:
                            jassertfalse;
                    }
                }

                // Dispatch to the method registry
                root->methodRegistry[fnIndex](
                    juce::var::NativeFunctionArgs(
                        juce::var(),
                        args.data(),
                        static_cast<int>(args.size())
                    )
                );

                return 0;
            }, DUK_VARARGS, 0, static_cast<unsigned int>(fnIndex));

            // Assign it to __BlueprintNative__
            duk_put_prop_string(ctx, -2, name.c_str());
        }

        /** Dispatches an event to the React internal view registry.

            If the view given by the `viewId` has a handler for the given event, it
            will be called with the given arguments.
         */
        template <typename... T>
        void dispatchViewEvent (ViewId viewId, const juce::String& eventType, T... args)
        {
            jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
            std::vector<juce::var> vargs { args... };

            // Push the dispatchViewEvent function to the top of the stack
            duk_push_global_object(ctx);
            duk_push_string(ctx, "__BlueprintNative__");
            duk_get_prop(ctx, -2);
            duk_push_string(ctx, "dispatchViewEvent");
            duk_get_prop(ctx, -2);

            // Now push the arguments
            int numArgs = 2 + static_cast<int>(vargs.size());
            duk_require_stack_top(ctx, numArgs);
            duk_push_int(ctx, viewId);
            duk_push_string(ctx, eventType.toRawUTF8());

            for (auto& p : vargs)
                pushVarToDukStack(p);

            // Then issue the call and clear the stack
            if (duk_pcall(ctx, numArgs) != DUK_EXEC_SUCCESS) {
                // If we have an error object at the top of the stack, we'll print the
                // stack property.
                if (duk_is_error(ctx, -1))
                {
                    // Accessing .stack might cause an error to be thrown, so wrap this
                    // access in a duk_safe_call() if it matters.
                    duk_get_prop_string(ctx, -1, "stack");
                    DBG("Duktape call error: " << duk_safe_to_string(ctx, -1));
                    duk_pop(ctx);
                }
                else
                {
                    // If it's not an error object we'll just coerce to string
                    DBG("Duktape call error: " << duk_safe_to_string(ctx, -1));
                }
            }

            duk_pop_n(ctx, 3);
        }

        /** Dispatches an event through the JavaScript EventBridge. */
        template <typename... T>
        void dispatchEvent (const juce::String& eventType, T... args)
        {
            jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
            std::vector<juce::var> vargs { args... };

            // Push the dispatchEvent function to the top of the stack
            duk_push_global_object(ctx);
            duk_push_string(ctx, "__BlueprintNative__");
            duk_get_prop(ctx, -2);
            duk_push_string(ctx, "dispatchEvent");
            duk_get_prop(ctx, -2);

            // Now push the arguments
            int numArgs = 1 + static_cast<int>(vargs.size());
            duk_require_stack_top(ctx, numArgs);
            duk_push_string(ctx, eventType.toRawUTF8());

            for (auto& p : vargs)
                pushVarToDukStack(p);

            // Then issue the call and clear the stack
            if (duk_pcall(ctx, numArgs) != DUK_EXEC_SUCCESS) {
                // If we have an error object at the top of the stack, we'll print the
                // stack property.
                if (duk_is_error(ctx, -1))
                {
                    // Accessing .stack might cause an error to be thrown, so wrap this
                    // access in a duk_safe_call() if it matters.
                    duk_get_prop_string(ctx, -1, "stack");
                    DBG("Duktape call error: " << duk_safe_to_string(ctx, -1));
                    duk_pop(ctx);
                }
                else
                {
                    // If it's not an error object we'll just coerce to string
                    DBG("Duktape call error: " << duk_safe_to_string(ctx, -1));
                }
            }

            duk_pop_n(ctx, 3);
        }

        void pushVarToDukStack (const juce::var& v)
        {
            if (v.isBool())
                return duk_push_boolean(ctx, (bool) v);
            if (v.isInt() || v.isInt64())
                return duk_push_int(ctx, (int) v);
            if (v.isDouble())
                return duk_push_number(ctx, (double) v);
            if (v.isString())
                return (void) duk_push_string(ctx, v.toString().toRawUTF8());
            if (v.isArray())
            {
                duk_idx_t arr_idx = duk_push_array(ctx);
                int i = 0;

                for (auto& e : *(v.getArray()))
                {
                    pushVarToDukStack(e);
                    duk_put_prop_index(ctx, arr_idx, i++);
                }

                return;
            }
            if (v.isObject())
            {
                if (auto* o = v.getDynamicObject())
                {
                    duk_idx_t obj_idx = duk_push_object(ctx);

                    for (auto& e : o->getProperties())
                    {
                        pushVarToDukStack(e.value);
                        duk_put_prop_string(ctx, obj_idx, e.name.toString().toRawUTF8());
                    }
                }

                return;
            }
        }

        static juce::var readVarFromDukStack (duk_context* ctx, duk_idx_t idx)
        {
            juce::var value;

            switch (duk_get_type(ctx, idx))
            {
                case DUK_TYPE_NULL:
                    // It looks like juce::var doesn't have an explicit null value,
                    // so we're just using the default empty constructor value.
                    break;
                case DUK_TYPE_UNDEFINED:
                    value = juce::var::undefined();
                    break;
                case DUK_TYPE_BOOLEAN:
                    value = (bool) duk_get_boolean(ctx, idx);
                    break;
                case DUK_TYPE_NUMBER:
                    value = duk_get_number(ctx, idx);
                    break;
                case DUK_TYPE_STRING:
                    value = duk_get_string(ctx, idx);
                    break;
                case DUK_TYPE_OBJECT:
                {
                    if (duk_is_array(ctx, idx))
                    {
                        duk_size_t len = duk_get_length(ctx, idx);
                        juce::Array<juce::var> els;

                        for (duk_size_t i = 0; i < len; ++i)
                        {
                            duk_get_prop_index(ctx, idx, i);
                            els.add(readVarFromDukStack(ctx, -1));
                            duk_pop(ctx);
                        }

                        value = els;
                        break;
                    }
                    else
                    {
                        juce::DynamicObject obj;

                        // Generic object enumeration; `duk_enum` pushes an enumerator
                        // object to the top of the stack
                        duk_enum(ctx, idx, DUK_ENUM_OWN_PROPERTIES_ONLY);

                        while (duk_next(ctx, -1, 1))
                        {
                            // For each found key/value pair, `duk_enum` pushes the
                            // values to the top of the stack. So here the stack top
                            // is [ ... enum key value]. Enum is at -3, key at -2,
                            // value at -1 from the stack top.
                            // Note here that all keys in an ECMAScript object are of
                            // type string, even arrays, e.g. `myArr[0]` has an implicit
                            // conversion from number to string. Thus here, while constructing
                            // the DynamicObject, we take the `toString()` value for the key
                            // always.
                            obj.setProperty(duk_to_string(ctx, -2), readVarFromDukStack(ctx, -1));

                            // Clear the key/value pair from the stack
                            duk_pop_2(ctx);
                        }

                        // Pop the enumerator from the stack
                        duk_pop(ctx);

                        value = juce::var(obj.clone());
                        break;
                    }
                }
                case DUK_TYPE_NONE:
                default:
                    jassertfalse;
            }

            return value;
        }

        /** Recursively computes the shadow tree layout, then traverses the tree
            flushing new layout bounds to the associated view components.
         */
        void performShadowTreeLayout()
        {
            juce::Rectangle<float> bounds = getLocalBounds().toFloat();
            const float width = bounds.getWidth();
            const float height = bounds.getHeight();

            _shadowView->computeViewLayout(width, height);
            _shadowView->flushViewLayout();
        }

        //==============================================================================
        std::vector<std::function<void(const juce::var::NativeFunctionArgs&)>> methodRegistry;

    private:
        //==============================================================================
        /** Registers each of the natively supported view types. */
        void installNativeViewTypes()
        {
            registerViewType("Text", []() -> ViewPair {
                auto view = std::make_unique<TextView>();
                auto shadowView = std::make_unique<TextShadowView>(view.get());

                return {std::move(view), std::move(shadowView)};
            });

            registerViewType("View", []() -> ViewPair {
                auto view = std::make_unique<View>();
                auto shadowView = std::make_unique<ShadowView>(view.get());

                return {std::move(view), std::move(shadowView)};
            });

            registerViewType("Image", []() -> ViewPair {
                auto view = std::make_unique<ImageView>();

                // ImageView does not need a specialized shadow view, unless
                // we want to enforce at the ShadowView level that it cannot
                // take children.
                auto shadowView = std::make_unique<ShadowView>(view.get());

                return {std::move(view), std::move(shadowView)};
            });

            registerViewType("ScrollView", []() -> ViewPair {
                auto view = std::make_unique<ScrollView>();
                auto shadowView = std::make_unique<ShadowView>(view.get());

                return {std::move(view), std::move(shadowView)};
            });

            registerViewType("ScrollViewContentView", []() -> ViewPair {
                auto view = std::make_unique<View>();
                auto shadowView = std::make_unique<ScrollViewContentShadowView>(view.get());

                return {std::move(view), std::move(shadowView)};
            });
        }

        //==============================================================================
        std::unique_ptr<ShadowView> _shadowView;
        std::map<ViewId, std::unique_ptr<View>> viewTable;
        std::map<ViewId, std::unique_ptr<ShadowView>> shadowViewTable;
        std::map<juce::String, ViewFactory> viewFactories;

        juce::File sourceFile;
        duk_context* ctx;

        //==============================================================================
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReactApplicationRoot)
    };

}

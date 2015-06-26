#include "edge.h"

CoreClrFunc::CoreClrFunc()
{
	functionHandle = NULL;
}

NAN_METHOD(coreClrFuncProxy)
{
    DBG("coreClrFuncProxy");
    NanEscapableScope();
    Handle<v8::External> correlator = Handle<v8::External>::Cast(args[2]);
    CoreClrFuncWrap* wrap = (CoreClrFuncWrap*)(correlator->Value());
    CoreClrFunc* clrFunc = wrap->clrFunc;
    NanReturnValue(clrFunc->Call(args[0], args[1]));
}

NAN_WEAK_CALLBACK(coreClrFuncProxyNearDeath)
{
    DBG("coreClrFuncProxyNearDeath");
    CoreClrFuncWrap* wrap = (CoreClrFuncWrap*)(data.GetParameter());
    wrap->clrFunc = NULL;
    delete wrap;
}

Handle<v8::Function> CoreClrFunc::InitializeInstance(CoreClrGcHandle functionHandle)
{
    DBG("CoreClrFunc::InitializeInstance - Started");

    static Persistent<v8::Function> proxyFactory;
    static Persistent<v8::Function> proxyFunction;

    NanEscapableScope();

    CoreClrFunc* app = new CoreClrFunc();
    app->functionHandle = functionHandle;
    CoreClrFuncWrap* wrap = new CoreClrFuncWrap();
    wrap->clrFunc = app;

    // See https://github.com/tjanczuk/edge/issues/128 for context

    if (proxyFactory.IsEmpty())
    {
    	DBG("CoreClrFunc::InitializeInstance - Creating proxy factory");

        NanAssignPersistent(
            proxyFunction, NanNew<FunctionTemplate>(coreClrFuncProxy)->GetFunction());
        Handle<v8::String> code = NanNew<v8::String>(
            "(function (f, ctx) { return function (d, cb) { return f(d, cb, ctx); }; })");
        NanAssignPersistent(
            proxyFactory, Handle<v8::Function>::Cast(v8::Script::Compile(code)->Run()));
    }

    Handle<v8::Value> factoryArgv[] = { NanNew(proxyFunction), NanNew<v8::External>((void*)wrap) };
    Local<v8::Function> funcProxy =
            (NanNew(proxyFactory)->Call(NanGetCurrentContext()->Global(), 2, factoryArgv)).As<v8::Function>();
    NanMakeWeakPersistent(funcProxy, (void*)wrap, &coreClrFuncProxyNearDeath);

    DBG("CoreClrFunc::InitializeInstance - Finished");

    return NanEscapeScope(funcProxy);
}

Handle<v8::Value> CoreClrFunc::Call(Handle<v8::Value> payload, Handle<v8::Value> callbackOrSync)
{
	DBG("CoreClrFunc::Call - Started");
	NanEscapableScope();

	void* marshalData;
	int payloadType;
	int taskState;
	void* result;
	int resultType;

	DBG("CoreClrFunc::Call - Marshalling data in preparating for calling the CLR");

	MarshalV8ToCLR(payload, &marshalData, &payloadType);
	DBG("CoreClrFunc::Call - Object type of %d is being marshalled", payloadType);

	DBG("CoreClrFunc::Call - Calling CoreClrEmbedding::CallClrFunc()");
	CoreClrEmbedding::CallClrFunc(functionHandle, marshalData, payloadType, &taskState, &result, &resultType);
	DBG("CoreClrFunc::Call - CoreClrEmbedding::CallClrFunc() returned a task state of %d", taskState);

	DBG("CoreClrFunc::Call - Freeing the marshalled data");
	FreeMarshalData(marshalData, payloadType);

	if (taskState == TaskStatus::RanToCompletion)
	{
		DBG("CoreClrFunc::Call - Task ran synchronously, marshalling CLR data for the callback");
		// TODO: marshal data to V8 and invoke the callback or return the data
	}

	else if (taskState == TaskStatus::Faulted)
	{
		DBG("CoreClrFunc::Call - Task threw an exception, marshalling CLR exception data for the callback");
		// TODO: marshal exception info and pass it to the callback
	}

	else if (callbackOrSync->IsBoolean())
	{
		DBG("CoreClrFunc::Call - Task was expected to run synchronously, but did not run to completion");
		// TODO: throw error for async being returned when we expected sync
	}

	else
	{
		DBG("CoreClrFunc::Call - Task running asynchronously, registering callback");

		CoreClrGcHandle taskHandle = result;
		CoreClrFuncInvokeContext* invokeContext = new CoreClrFuncInvokeContext(callbackOrSync, taskHandle);

		invokeContext->InitializeAsyncOperation();
		CoreClrEmbedding::ContinueTask(taskHandle, invokeContext, CoreClrFuncInvokeContext::TaskComplete);
	}

	DBG("CoreClrFunc::Call - Finished");

	return NanEscapeScope(NanUndefined());
}

NAN_METHOD(CoreClrFunc::Initialize)
{
	DBG("CoreClrFunc::Initialize - Starting");

	NanEscapableScope();
	Handle<v8::Object> options = args[0]->ToObject();
	Handle<v8::Function> result;

	Handle<v8::Value> assemblyFileArgument = options->Get(NanNew<v8::String>("assemblyFile"));

	if (assemblyFileArgument->IsString())
	{
		v8::String::Utf8Value assemblyFile(assemblyFileArgument);
		v8::String::Utf8Value typeName(options->Get(NanNew<v8::String>("typeName")));
		v8::String::Utf8Value methodName(options->Get(NanNew<v8::String>("methodName")));

		DBG("CoreClrFunc::Initialize - Loading %s.%s() from %s", *typeName, *methodName, *assemblyFile);
		CoreClrGcHandle functionHandle = CoreClrEmbedding::GetClrFuncReflectionWrapFunc(*assemblyFile, *typeName, *methodName);
		DBG("CoreClrFunc::Initialize - Function loaded successfully")

		result = CoreClrFunc::InitializeInstance(functionHandle);
		DBG("CoreClrFunc::Initialize - Callback initialized successfully")
	}

	else
	{
		// TODO: support compilation from source once the Roslyn C# compiler is made available on CoreCLR
		throwV8Exception("Compiling .NET methods from source is not yet supported with CoreCLR, you must provide an assembly path, type name, and method name as arguments to edge.initializeClrFunction().");
	}

	DBG("CoreClrFunc::Initialize - Finished");

	NanReturnValue(result);
}

void CoreClrFunc::FreeMarshalData(void* marshalData, int payloadType)
{
	switch (payloadType)
	{
		case V8Type::PropertyTypeString:
			delete ((char*)marshalData);
			break;

		case V8Type::PropertyTypeObject:
			delete ((V8ObjectData*)marshalData);
			break;

		case V8Type::PropertyTypeBoolean:
			delete ((bool*)marshalData);
			break;

		case V8Type::PropertyTypeNumber:
		case V8Type::PropertyTypeDate:
			delete ((double*)marshalData);
			break;

		case V8Type::PropertyTypeInt32:
			delete ((int32_t*)marshalData);
			break;

		case V8Type::PropertyTypeUInt32:
			delete ((uint32_t*)marshalData);
			break;

		case V8Type::PropertyTypeNull:
			break;

		case V8Type::PropertyTypeArray:
			delete ((V8ArrayData*)marshalData);
			break;
	}
}

char* CoreClrFunc::CopyV8StringBytes(Handle<v8::String> v8String)
{
	String::Utf8Value utf8String(v8String);
	char* sourceBytes = *utf8String;
	int sourceLength = strlen(sourceBytes);
	char* destinationBytes = new char[sourceLength + 1];

	strncpy(destinationBytes, sourceBytes, sourceLength);
	destinationBytes[sourceLength] = '\0';

	return destinationBytes;
}

void CoreClrFunc::MarshalV8ToCLR(Handle<v8::Value> jsdata, void** marshalData, int* payloadType)
{
	if (jsdata->IsString())
	{
		*marshalData = CopyV8StringBytes(Handle<v8::String>::Cast(jsdata));
		*payloadType = V8Type::PropertyTypeString;
	}

	else if (jsdata->IsFunction())
	{
		// TODO: implement
	}

	else if (node::Buffer::HasInstance(jsdata))
	{
		// TODO: implement
	}

	else if (jsdata->IsArray())
	{
		Handle<v8::Array> jsarray = Handle<v8::Array>::Cast(jsdata);
		V8ArrayData* arrayData = new V8ArrayData();

		arrayData->arrayLength = jsarray->Length();
		arrayData->itemTypes = new int[arrayData->arrayLength];
		arrayData->itemValues = new void*[arrayData->arrayLength];

		for (int i = 0; i < arrayData->arrayLength; i++)
		{
			MarshalV8ToCLR(jsarray->Get(i), &arrayData->itemValues[i], &arrayData->itemTypes[i]);
		}

		*marshalData = arrayData;
		*payloadType = V8Type::PropertyTypeArray;
	}

	else if (jsdata->IsDate())
	{
		Handle<v8::Date> jsdate = Handle<v8::Date>::Cast(jsdata);
		double* ticks = new double();

		*ticks = jsdate->NumberValue();
		*marshalData = ticks;
		*payloadType = V8Type::PropertyTypeDate;
	}

	else if (jsdata->IsBoolean())
	{
		bool* value = new bool();
		*value = jsdata->BooleanValue();

		*marshalData = value;
		*payloadType = V8Type::PropertyTypeBoolean;
	}

	else if (jsdata->IsInt32())
	{
		int32_t* value = new int32_t();
		*value = jsdata->Int32Value();

		*marshalData = value;
		*payloadType = V8Type::PropertyTypeInt32;
	}

	else if (jsdata->IsUint32())
	{
		uint32_t* value = new uint32_t();
		*value = jsdata->Uint32Value();

		*marshalData = value;
		*payloadType = V8Type::PropertyTypeUInt32;
	}

	else if (jsdata->IsNumber())
	{
		double* value = new double();
		*value = jsdata->NumberValue();

		*marshalData = value;
		*payloadType = V8Type::PropertyTypeNumber;
	}

	else if (jsdata->IsUndefined() || jsdata->IsNull())
	{
		*payloadType = V8Type::PropertyTypeNull;
	}

	else if (jsdata->IsObject())
	{
		V8ObjectData* objectData = new V8ObjectData();

		Handle<v8::Object> jsobject = Handle<v8::Object>::Cast(jsdata);
		Handle<v8::Array> propertyNames = jsobject->GetPropertyNames();

		objectData->propertiesCount = propertyNames->Length();
		objectData->propertyData = new void*[objectData->propertiesCount];
		objectData->propertyNames = new char*[objectData->propertiesCount];
		objectData->propertyTypes = new int[objectData->propertiesCount];

		for (unsigned int i = 0; i < propertyNames->Length(); i++)
		{
			Handle<v8::String> name = Handle<v8::String>::Cast(propertyNames->Get(i));

			objectData->propertyNames[i] = CopyV8StringBytes(name);
			MarshalV8ToCLR(jsobject->Get(name), &objectData->propertyData[i], &objectData->propertyTypes[i]);
		}

		*marshalData = objectData;
		*payloadType = V8Type::PropertyTypeObject;
	}
}

Handle<v8::Value> CoreClrFunc::MarshalCLRToV8(void* marshalData, int payloadType)
{
	NanEscapableScope();

	if (payloadType == V8Type::PropertyTypeString)
	{
		return NanEscapeScope(NanNew<v8::String>((char*) marshalData));
	}

	else if (payloadType == V8Type::PropertyTypeInt32)
	{
		return NanEscapeScope(NanNew<v8::Integer>(*(int*) marshalData));
	}

	else if (payloadType == V8Type::PropertyTypeNumber)
	{
		return NanEscapeScope(NanNew<v8::Number>(*(double*) marshalData));
	}

	else if (payloadType == V8Type::PropertyTypeDate)
	{
		return NanEscapeScope(NanNew<v8::Date>(*(double*) marshalData));
	}

	else if (payloadType == V8Type::PropertyTypeBoolean)
	{
		bool value = (*(int*) marshalData) != 0;
		return NanEscapeScope(NanNew<v8::Boolean>(value));
	}

	else if (payloadType == V8Type::PropertyTypeArray)
	{
		V8ArrayData* arrayData = (V8ArrayData*) marshalData;
		Handle<v8::Array> result = NanNew<v8::Array>();

		for (int i = 0; i < arrayData->arrayLength; i++)
		{
			result->Set(i, MarshalCLRToV8(arrayData->itemValues[i], arrayData->itemTypes[i]));
		}

		return NanEscapeScope(result);
	}

	else if (payloadType == V8Type::PropertyTypeObject)
	{
		V8ObjectData* objectData = (V8ObjectData*) marshalData;
		Handle<v8::Object> result = NanNew<v8::Object>();

		for (int i = 0; i < objectData->propertiesCount; i++)
		{
			result->Set(NanNew<v8::String>(objectData->propertyNames[i]), MarshalCLRToV8(objectData->propertyData[i], objectData->propertyTypes[i]));
		}

		return NanEscapeScope(result);
	}

	else if (payloadType == V8Type::PropertyTypeNull)
	{
		return NanEscapeScope(NanNull());
	}

	else
	{
		throwV8Exception("Unsupported object type received from the CLR: %d", payloadType);
		return NanEscapeScope(NanUndefined());
	}
}

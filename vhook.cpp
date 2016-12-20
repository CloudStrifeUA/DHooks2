#include "vhook.h"
#include "vfunc_call.h"
#include "util.h"

SourceHook::IHookManagerAutoGen *g_pHookManager = NULL;

ke::Vector<DHooksManager *> g_pHooks;

using namespace SourceHook;

#ifdef  WIN32
#define OBJECT_OFFSET sizeof(void *)
#else
#define OBJECT_OFFSET (sizeof(void *)*2)
#endif

DHooksManager::DHooksManager(HookSetup *setup, void *iface, IPluginFunction *remove_callback, bool post)
{
	this->callback = MakeHandler(setup->returnType);
	this->hookid = 0;
	this->remove_callback = remove_callback;
	this->callback->offset = setup->offset;
	this->callback->plugin_callback = setup->callback;
	this->callback->returnFlag = setup->returnFlag;
	this->callback->thisType = setup->thisType;
	this->callback->post = post;
	this->callback->hookType = setup->hookType;
	this->callback->params = setup->params;

	this->addr = 0;

	if(this->callback->hookType == HookType_Entity)
	{
		this->callback->entity = gamehelpers->EntityToBCompatRef((CBaseEntity *)iface);
	}
	else
	{
		if(this->callback->hookType == HookType_Raw)
		{
			this->addr = (intptr_t)iface;
		}
		this->callback->entity = -1;
	}

	CProtoInfoBuilder protoInfo(ProtoInfo::CallConv_ThisCall);

	for(int i = this->callback->params.size() -1; i >= 0; i--)
	{
		protoInfo.AddParam(this->callback->params.at(i).size, this->callback->params.at(i).pass_type, PASSFLAG_BYVAL, NULL, NULL, NULL, NULL);//This seems like we need to do something about it at some point...
	}

	if(this->callback->returnType == ReturnType_Void)
	{
		protoInfo.SetReturnType(0, SourceHook::PassInfo::PassType_Unknown, 0, NULL, NULL, NULL, NULL);
	}
	else if(this->callback->returnType == ReturnType_Float)
	{
		protoInfo.SetReturnType(sizeof(float), SourceHook::PassInfo::PassType_Float, setup->returnFlag, NULL, NULL, NULL, NULL);
	}
	else if(this->callback->returnType == ReturnType_String)
	{
		protoInfo.SetReturnType(sizeof(string_t), SourceHook::PassInfo::PassType_Object, setup->returnFlag, NULL, NULL, NULL, NULL);//We have to be 4 really... or else RIP
	}
	else if(this->callback->returnType == ReturnType_Vector)
	{
		protoInfo.SetReturnType(sizeof(SDKVector), SourceHook::PassInfo::PassType_Object, setup->returnFlag, NULL, NULL, NULL, NULL);
	}
	else
	{
		protoInfo.SetReturnType(sizeof(void *), SourceHook::PassInfo::PassType_Basic, setup->returnFlag, NULL, NULL, NULL, NULL);
	}
	this->pManager = g_pHookManager->MakeHookMan(protoInfo, 0, this->callback->offset);

	this->hookid = g_SHPtr->AddHook(g_PLID,ISourceHook::Hook_Normal, iface, 0, this->pManager, this->callback, this->callback->post);
}

void CleanupHooks(IPluginContext *pContext)
{
	for(int i = g_pHooks.length() -1; i >= 0; i--)
	{
		DHooksManager *manager = g_pHooks.at(i);

		if(pContext == NULL || pContext == manager->callback->plugin_callback->GetParentRuntime()->GetDefaultContext())
		{
			delete manager;
			g_pHooks.remove(i);
		}
	}
}

bool SetupHookManager(ISmmAPI *ismm)
{
	g_pHookManager = static_cast<SourceHook::IHookManagerAutoGen *>(ismm->MetaFactory(MMIFACE_SH_HOOKMANAUTOGEN, NULL, NULL));
	
	return g_pHookManager != NULL;
}

SourceHook::PassInfo::PassType GetParamTypePassType(HookParamType type)
{
	switch(type)
	{
		case HookParamType_Float:
			return SourceHook::PassInfo::PassType_Float;
		case HookParamType_Object:
			return SourceHook::PassInfo::PassType_Object;
	}
	return SourceHook::PassInfo::PassType_Basic;
}

size_t GetStackArgsSize(DHooksCallback *dg)
{
	size_t res = GetParamsSize(dg);
	#ifdef  WIN32
	if(dg->returnType == ReturnType_Vector)//Account for result vector ptr.
	#else
	if(dg->returnType == ReturnType_Vector || dg->returnType == ReturnType_String)
	#endif
	{
		res += OBJECT_OFFSET;
	}
	return res;
}

HookParamsStruct::~HookParamsStruct()
{
	if (this->orgParams != NULL)
	{
		free(this->orgParams);
	}
	if (this->isChanged != NULL)
	{
		free(this->isChanged);
	}
	if (this->newParams != NULL)
	{
		for (int i = dg->params.size() - 1; i >= 0; i--)
		{
			size_t offset = GetParamOffset(this, i);
			void *addr = (void **)((intptr_t)this->newParams + offset);

			if (*(void **)addr == NULL)
				continue;

			if (dg->params.at(i).type == HookParamType_VectorPtr)
			{
				delete *(SDKVector **)addr;
			}
			else if (dg->params.at(i).type == HookParamType_CharPtr)
			{
				delete *(char **)addr;
			}
		}
		free(this->newParams);
	}
}

HookParamsStruct *GetParamStruct(DHooksCallback *dg, void **argStack, size_t argStackSize)
{
	HookParamsStruct *params = new HookParamsStruct();
	params->dg = dg;
	#ifdef  WIN32
	if(dg->returnType != ReturnType_Vector)
	#else
	if(dg->returnType != ReturnType_Vector && dg->returnType != ReturnType_String)
	#endif
	{
		params->orgParams = (void **)malloc(argStackSize);
		memcpy(params->orgParams, argStack, argStackSize);
	}
	else //Offset result ptr
	{
		params->orgParams = (void **)malloc(argStackSize-OBJECT_OFFSET);
		memcpy(params->orgParams, argStack+OBJECT_OFFSET, argStackSize-OBJECT_OFFSET);
	}
	size_t paramsSize = GetParamsSize(dg);

	params->newParams = (void **)malloc(paramsSize);
	params->isChanged = (bool *)malloc(dg->params.size() * sizeof(bool));

	for (unsigned int i = 0; i < dg->params.size(); i++)
	{
		*(void **)((intptr_t)params->newParams + GetParamOffset(params, i)) = NULL;
		params->isChanged[i] = false;
	}

	return params;
}

HookReturnStruct *GetReturnStruct(DHooksCallback *dg)
{
	HookReturnStruct *res = new HookReturnStruct();
	res->isChanged = false;
	res->type = dg->returnType;
	res->orgResult = NULL;
	res->newResult = NULL;

	if(g_SHPtr->GetOrigRet() && dg->post)
	{
		switch(dg->returnType)
		{
			case ReturnType_String:
				res->orgResult = malloc(sizeof(string_t));
				res->newResult = malloc(sizeof(string_t));
				*(string_t *)res->orgResult = META_RESULT_ORIG_RET(string_t);
				break;
			case ReturnType_Int:
				res->orgResult = malloc(sizeof(int));
				res->newResult = malloc(sizeof(int));
				*(int *)res->orgResult = META_RESULT_ORIG_RET(int);
				break;
			case ReturnType_Bool:
				res->orgResult = malloc(sizeof(bool));
				res->newResult = malloc(sizeof(bool));
				*(bool *)res->orgResult = META_RESULT_ORIG_RET(bool);
				break;
			case ReturnType_Float:
				res->orgResult = malloc(sizeof(float));
				res->newResult = malloc(sizeof(float));
				*(float *)res->orgResult = META_RESULT_ORIG_RET(float);
				break;
			case ReturnType_Vector:
			{
				res->orgResult = malloc(sizeof(SDKVector));
				res->newResult = malloc(sizeof(SDKVector));
				SDKVector vec = META_RESULT_ORIG_RET(SDKVector);
				*(SDKVector *)res->orgResult = vec;
				break;
			}
			default:
				res->orgResult = META_RESULT_ORIG_RET(void *);
				break;
		}
	}
	else
	{
		switch(dg->returnType)
		{
			case ReturnType_String:
				res->orgResult = malloc(sizeof(string_t));
				res->newResult = malloc(sizeof(string_t));
				*(string_t *)res->orgResult = NULL_STRING;
				break;
			case ReturnType_Vector:
				res->orgResult = malloc(sizeof(SDKVector));
				res->newResult = malloc(sizeof(SDKVector));
				*(SDKVector *)res->orgResult = SDKVector();
				break;
			case ReturnType_Int:
				res->orgResult = malloc(sizeof(int));
				res->newResult = malloc(sizeof(int));
				*(int *)res->orgResult = 0;
				break;
			case ReturnType_Bool:
				res->orgResult = malloc(sizeof(bool));
				res->newResult = malloc(sizeof(bool));
				*(bool *)res->orgResult = false;
				break;
			case ReturnType_Float:
				res->orgResult = malloc(sizeof(float));
				res->newResult = malloc(sizeof(float));
				*(float *)res->orgResult = 0.0;
				break;
		}
	}

	return res;
}

cell_t GetThisPtr(void *iface, ThisPointerType type)
{
	if(type == ThisPointer_CBaseEntity)
	{
		return gamehelpers->EntityToBCompatRef((CBaseEntity *)iface);
	}

	return (cell_t)iface;
}

#ifdef  WIN32
void *Callback(DHooksCallback *dg, void **argStack, size_t *argsizep)
#else
void *Callback(DHooksCallback *dg, void **argStack)
#endif
{
	HookReturnStruct *returnStruct = NULL;
	HookParamsStruct *paramStruct = NULL;
	Handle_t rHndl;
	Handle_t pHndl;

	#ifdef  WIN32
	*argsizep = GetStackArgsSize(dg);
	#else
	size_t argsize = GetStackArgsSize(dg);
	#endif

	if(dg->thisType == ThisPointer_CBaseEntity || dg->thisType == ThisPointer_Address)
	{
		dg->plugin_callback->PushCell(GetThisPtr(g_SHPtr->GetIfacePtr(), dg->thisType));
	}
	if(dg->returnType != ReturnType_Void)
	{
		returnStruct = GetReturnStruct(dg);
		rHndl = handlesys->CreateHandle(g_HookReturnHandle, returnStruct, dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity(), NULL);
		if(!rHndl)
		{
			dg->plugin_callback->Cancel();
			if(returnStruct)
			{
				delete returnStruct;
			}
			g_SHPtr->SetRes(MRES_IGNORED);
			return NULL;
		}
		dg->plugin_callback->PushCell(rHndl);
	}

	#ifdef  WIN32
	if(*argsizep > 0)
	{
		paramStruct = GetParamStruct(dg, argStack, *argsizep);
	#else
	if(argsize > 0)
	{
		paramStruct = GetParamStruct(dg, argStack, argsize);
	#endif
		pHndl = handlesys->CreateHandle(g_HookParamsHandle, paramStruct, dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity(), NULL);
		if(!pHndl)
		{
			dg->plugin_callback->Cancel();
			if(returnStruct)
			{
				delete returnStruct;
			}
			if(paramStruct)
			{
				delete paramStruct;
			}
			g_SHPtr->SetRes(MRES_IGNORED);
			return NULL;
		}
		dg->plugin_callback->PushCell(pHndl);
	}
	cell_t result = (cell_t)MRES_Ignored;
	META_RES mres = MRES_IGNORED;

	dg->plugin_callback->Execute(&result);

	void *ret = g_SHPtr->GetOverrideRetPtr();
	switch((MRESReturn)result)
	{
		case MRES_Handled:
		case MRES_ChangedHandled:
			g_SHPtr->DoRecall();
			g_SHPtr->SetRes(MRES_SUPERCEDE);
			mres = MRES_SUPERCEDE;
			ret = CallVFunction<void *>(dg, paramStruct, g_SHPtr->GetIfacePtr());
			g_SHPtr->EndContext(NULL);
			break;
		case MRES_ChangedOverride:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					if(dg->returnType == ReturnType_String || dg->returnType == ReturnType_Int || dg->returnType == ReturnType_Bool)
					{
						ret = *(void **)returnStruct->newResult;
					}
					else
					{
						ret = returnStruct->newResult;
					}
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
					break;
				}
			}
			g_SHPtr->DoRecall();
			g_SHPtr->SetRes(MRES_SUPERCEDE);
			mres = MRES_SUPERCEDE;
			CallVFunction<void *>(dg, paramStruct, g_SHPtr->GetIfacePtr());
			g_SHPtr->EndContext(NULL);
			break;
		case MRES_Override:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					g_SHPtr->SetRes(MRES_OVERRIDE);
					mres = MRES_OVERRIDE;
					if(dg->returnType == ReturnType_String || dg->returnType == ReturnType_Int || dg->returnType == ReturnType_Bool)
					{
						ret = *(void **)returnStruct->newResult;
					}
					else
					{
						ret = returnStruct->newResult;
					}
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
				}
			}
			break;
		case MRES_Supercede:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					g_SHPtr->SetRes(MRES_SUPERCEDE);
					mres = MRES_SUPERCEDE;
					if(dg->returnType == ReturnType_String || dg->returnType == ReturnType_Int || dg->returnType == ReturnType_Bool)
					{
						ret = *(void **)returnStruct->newResult;
					}
					else
					{
						ret = returnStruct->newResult;
					}
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
				}
			}
			else
			{
				g_SHPtr->DoRecall();
				g_SHPtr->SetRes(MRES_SUPERCEDE);
				mres = MRES_SUPERCEDE;
				g_SHPtr->EndContext(NULL);
			}
			break;
		default:
			g_SHPtr->SetRes(MRES_IGNORED);
			mres = MRES_IGNORED;
			break;
	}

	HandleSecurity sec(dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity());

	if(returnStruct)
	{
		handlesys->FreeHandle(rHndl, &sec);
	}
	if(paramStruct)
	{
		handlesys->FreeHandle(pHndl, &sec);
	}

	if(dg->returnType == ReturnType_Void || mres <= MRES_HANDLED)
	{
		return NULL;
	}
	return ret;
}
#ifdef  WIN32
float Callback_float(DHooksCallback *dg, void **argStack, size_t *argsizep)
#else
float Callback_float(DHooksCallback *dg, void **argStack)
#endif
{
	HookReturnStruct *returnStruct = NULL;
	HookParamsStruct *paramStruct = NULL;
	Handle_t rHndl;
	Handle_t pHndl;

	#ifdef  WIN32
	*argsizep = GetStackArgsSize(dg);
	#else
	size_t argsize = GetStackArgsSize(dg);
	#endif

	if(dg->thisType == ThisPointer_CBaseEntity || dg->thisType == ThisPointer_Address)
	{
		dg->plugin_callback->PushCell(GetThisPtr(g_SHPtr->GetIfacePtr(), dg->thisType));
	}

	returnStruct = GetReturnStruct(dg);
	rHndl = handlesys->CreateHandle(g_HookReturnHandle, returnStruct, dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity(), NULL);

	if(!rHndl)
	{
		dg->plugin_callback->Cancel();
		if(returnStruct)
		{
			delete returnStruct;
		}
		g_SHPtr->SetRes(MRES_IGNORED);
		return 0.0;
	}
	dg->plugin_callback->PushCell(rHndl);

	#ifdef  WIN32
	if(*argsizep > 0)
	{
		paramStruct = GetParamStruct(dg, argStack, *argsizep);
	#else
	if(argsize > 0)
	{
		paramStruct = GetParamStruct(dg, argStack, argsize);
	#endif
		pHndl = handlesys->CreateHandle(g_HookParamsHandle, paramStruct, dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity(), NULL);
		if(!pHndl)
		{
			dg->plugin_callback->Cancel();
			if(returnStruct)
			{
				delete returnStruct;
			}
			if(paramStruct)
			{
				delete paramStruct;
			}
			g_SHPtr->SetRes(MRES_IGNORED);
			return 0.0;
		}
		dg->plugin_callback->PushCell(pHndl);
	}
	cell_t result = (cell_t)MRES_Ignored;
	META_RES mres = MRES_IGNORED;
	dg->plugin_callback->Execute(&result);

	void *ret = g_SHPtr->GetOverrideRetPtr();
	switch((MRESReturn)result)
	{
		case MRES_Handled:
		case MRES_ChangedHandled:
			g_SHPtr->DoRecall();
			g_SHPtr->SetRes(MRES_SUPERCEDE);
			mres = MRES_SUPERCEDE;
			*(float *)ret = CallVFunction<float>(dg, paramStruct, g_SHPtr->GetIfacePtr());
			g_SHPtr->EndContext(NULL);
			break;
		case MRES_ChangedOverride:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					*(float *)ret = *(float *)returnStruct->newResult;
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
					break;
				}
			}
			g_SHPtr->DoRecall();
			g_SHPtr->SetRes(MRES_SUPERCEDE);
			mres = MRES_SUPERCEDE;
			CallVFunction<float>(dg, paramStruct, g_SHPtr->GetIfacePtr());
			g_SHPtr->EndContext(NULL);
			break;
		case MRES_Override:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					g_SHPtr->SetRes(MRES_OVERRIDE);
					mres = MRES_OVERRIDE;
					*(float *)ret = *(float *)returnStruct->newResult;
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
				}
			}
			break;
		case MRES_Supercede:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					g_SHPtr->SetRes(MRES_SUPERCEDE);
					mres = MRES_SUPERCEDE;
					*(float *)ret = *(float *)returnStruct->newResult;
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
				}
			}
			break;
		default:
			g_SHPtr->SetRes(MRES_IGNORED);
			mres = MRES_IGNORED;
			break;
	}

	HandleSecurity sec(dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity());

	if(returnStruct)
	{
		handlesys->FreeHandle(rHndl, &sec);
	}
	if(paramStruct)
	{
		handlesys->FreeHandle(pHndl, &sec);
	}

	if(dg->returnType == ReturnType_Void || mres <= MRES_HANDLED)
	{
		return 0.0;
	}
	return *(float *)ret;
}
#ifdef  WIN32
SDKVector *Callback_vector(DHooksCallback *dg, void **argStack, size_t *argsizep)
#else
SDKVector *Callback_vector(DHooksCallback *dg, void **argStack)
#endif
{
	SDKVector *vec_result = (SDKVector *)argStack[0]; // Save the result

	HookReturnStruct *returnStruct = NULL;
	HookParamsStruct *paramStruct = NULL;
	Handle_t rHndl;
	Handle_t pHndl;

	#ifdef  WIN32
	*argsizep = GetStackArgsSize(dg);
	#else
	size_t argsize = GetStackArgsSize(dg);
	#endif

	if(dg->thisType == ThisPointer_CBaseEntity || dg->thisType == ThisPointer_Address)
	{
		dg->plugin_callback->PushCell(GetThisPtr(g_SHPtr->GetIfacePtr(), dg->thisType));
	}

	returnStruct = GetReturnStruct(dg);
	rHndl = handlesys->CreateHandle(g_HookReturnHandle, returnStruct, dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity(), NULL);

	if(!rHndl)
	{
		dg->plugin_callback->Cancel();
		if(returnStruct)
		{
			delete returnStruct;
		}
		g_SHPtr->SetRes(MRES_IGNORED);
		return NULL;
	}
	dg->plugin_callback->PushCell(rHndl);

	#ifdef  WIN32
	if(*argsizep > 0)
	{
		paramStruct = GetParamStruct(dg, argStack, *argsizep);
	#else
	if(argsize > 0)
	{
		paramStruct = GetParamStruct(dg, argStack, argsize);
	#endif
		pHndl = handlesys->CreateHandle(g_HookParamsHandle, paramStruct, dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity(), NULL);
		if(!pHndl)
		{
			dg->plugin_callback->Cancel();
			if(returnStruct)
			{
				delete returnStruct;
			}
			if(paramStruct)
			{
				delete paramStruct;
			}
			g_SHPtr->SetRes(MRES_IGNORED);
			return NULL;
		}
		dg->plugin_callback->PushCell(pHndl);
	}
	cell_t result = (cell_t)MRES_Ignored;
	META_RES mres = MRES_IGNORED;
	dg->plugin_callback->Execute(&result);

	void *ret = g_SHPtr->GetOverrideRetPtr();
	ret = vec_result;
	switch((MRESReturn)result)
	{
		case MRES_Handled:
		case MRES_ChangedHandled:
			g_SHPtr->DoRecall();
			g_SHPtr->SetRes(MRES_SUPERCEDE);
			mres = MRES_SUPERCEDE;
			*vec_result = CallVFunction<SDKVector>(dg, paramStruct, g_SHPtr->GetIfacePtr());
			g_SHPtr->EndContext(NULL);
			break;
		case MRES_ChangedOverride:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					*vec_result = **(SDKVector **)returnStruct->newResult;
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
					break;
				}
			}
			g_SHPtr->DoRecall();
			g_SHPtr->SetRes(MRES_SUPERCEDE);
			mres = MRES_SUPERCEDE;
			CallVFunction<SDKVector>(dg, paramStruct, g_SHPtr->GetIfacePtr());
			g_SHPtr->EndContext(NULL);
			break;
		case MRES_Override:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					g_SHPtr->SetRes(MRES_OVERRIDE);
					mres = MRES_OVERRIDE;
					*vec_result = **(SDKVector **)returnStruct->newResult;
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
				}
			}
			break;
		case MRES_Supercede:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					g_SHPtr->SetRes(MRES_SUPERCEDE);
					mres = MRES_SUPERCEDE;
					*vec_result = **(SDKVector **)returnStruct->newResult;
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
				}
			}
			break;
		default:
			g_SHPtr->SetRes(MRES_IGNORED);
			mres = MRES_IGNORED;
			break;
	}

	HandleSecurity sec(dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity());

	if(returnStruct)
	{
		handlesys->FreeHandle(rHndl, &sec);
	}
	if(paramStruct)
	{
		handlesys->FreeHandle(pHndl, &sec);
	}

	if(dg->returnType == ReturnType_Void || mres <= MRES_HANDLED)
	{
		vec_result->x = 0;
		vec_result->y = 0;
		vec_result->z = 0;
		return vec_result;
	}
	return vec_result;
}

#ifndef WIN32
string_t *Callback_stringt(DHooksCallback *dg, void **argStack)
{
	string_t *string_result = (string_t *)argStack[0]; // Save the result

	HookReturnStruct *returnStruct = NULL;
	HookParamsStruct *paramStruct = NULL;
	Handle_t rHndl;
	Handle_t pHndl;

	size_t argsize = GetStackArgsSize(dg);

	if(dg->thisType == ThisPointer_CBaseEntity || dg->thisType == ThisPointer_Address)
	{
		dg->plugin_callback->PushCell(GetThisPtr(g_SHPtr->GetIfacePtr(), dg->thisType));
	}

	returnStruct = GetReturnStruct(dg);
	rHndl = handlesys->CreateHandle(g_HookReturnHandle, returnStruct, dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity(), NULL);

	if(!rHndl)
	{
		dg->plugin_callback->Cancel();
		if(returnStruct)
		{
			delete returnStruct;
		}
		g_SHPtr->SetRes(MRES_IGNORED);
		return NULL;
	}
	dg->plugin_callback->PushCell(rHndl);

	if(argsize > 0)
	{
		paramStruct = GetParamStruct(dg, argStack, argsize);
		pHndl = handlesys->CreateHandle(g_HookParamsHandle, paramStruct, dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity(), NULL);
		if(!pHndl)
		{
			dg->plugin_callback->Cancel();
			if(returnStruct)
			{
				delete returnStruct;
			}
			if(paramStruct)
			{
				delete paramStruct;
			}
			g_SHPtr->SetRes(MRES_IGNORED);
			return NULL;
		}
		dg->plugin_callback->PushCell(pHndl);
	}
	cell_t result = (cell_t)MRES_Ignored;
	META_RES mres = MRES_IGNORED;
	dg->plugin_callback->Execute(&result);

	void *ret = g_SHPtr->GetOverrideRetPtr();
	ret = string_result;
	switch((MRESReturn)result)
	{
		case MRES_Handled:
		case MRES_ChangedHandled:
			g_SHPtr->DoRecall();
			g_SHPtr->SetRes(MRES_SUPERCEDE);
			mres = MRES_SUPERCEDE;
			*string_result = CallVFunction<string_t>(dg, paramStruct, g_SHPtr->GetIfacePtr());
			g_SHPtr->EndContext(NULL);
			break;
		case MRES_ChangedOverride:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					*string_result = *(string_t *)returnStruct->newResult;
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
					break;
				}
			}
			g_SHPtr->DoRecall();
			g_SHPtr->SetRes(MRES_SUPERCEDE);
			mres = MRES_SUPERCEDE;
			CallVFunction<SDKVector>(dg, paramStruct, g_SHPtr->GetIfacePtr());
			g_SHPtr->EndContext(NULL);
			break;
		case MRES_Override:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					g_SHPtr->SetRes(MRES_OVERRIDE);
					mres = MRES_OVERRIDE;
					*string_result = *(string_t *)returnStruct->newResult;
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
				}
			}
			break;
		case MRES_Supercede:
			if(dg->returnType != ReturnType_Void)
			{
				if(returnStruct->isChanged)
				{
					g_SHPtr->SetRes(MRES_SUPERCEDE);
					mres = MRES_SUPERCEDE;
					*string_result = *(string_t *)returnStruct->newResult;
				}
				else //Throw an error if no override was set
				{
					g_SHPtr->SetRes(MRES_IGNORED);
					mres = MRES_IGNORED;
					dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->BlamePluginError(dg->plugin_callback, "Tried to override return value without return value being set");
				}
			}
			break;
		default:
			g_SHPtr->SetRes(MRES_IGNORED);
			mres = MRES_IGNORED;
			break;
	}

	HandleSecurity sec(dg->plugin_callback->GetParentRuntime()->GetDefaultContext()->GetIdentity(), myself->GetIdentity());

	if(returnStruct)
	{
		handlesys->FreeHandle(rHndl, &sec);
	}
	if(paramStruct)
	{
		handlesys->FreeHandle(pHndl, &sec);
	}

	if(dg->returnType == ReturnType_Void || mres <= MRES_HANDLED)
	{
		*string_result = NULL_STRING;
		return string_result;
	}
	return string_result;
}
#endif

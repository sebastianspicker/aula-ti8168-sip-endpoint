static void finish_logout(ls200_gateway *gateway,ls200_gateway_session *session,const ls200_gateway_request *request,const route_plan *plan,ls200_gateway_response *response) { write_success(response,200U,"{\"logged_out\":true}"); save_mutation(gateway,session,request,plan,response); OPENSSL_cleanse(session,sizeof(*session)); (void)snprintf(response->set_cookie,sizeof(response->set_cookie),"ls200_session=; Path=/zoom/; Secure; HttpOnly; SameSite=Strict; Max-Age=0"); }
static int write_session_response(const ls200_gateway_session *session,ls200_gateway_response *response) {
  char data[160]={0},csrf[65]={0}; int result=0;
  if(hex_encode(session->csrf,sizeof(session->csrf),csrf,sizeof(csrf))) { (void)snprintf(data,sizeof(data),"{\"csrf_token\":\"%s\",\"role\":%u}",csrf,(unsigned)session->role); write_success(response,200U,data); result=1; }
  OPENSSL_cleanse(data,sizeof(data)); OPENSSL_cleanse(csrf,sizeof(csrf)); return result;
}
static void write_preview_metadata(ls200_gateway *gateway, ls200_gateway_response *response) {
  json_t *data = preview_status_data(gateway);
  char encoded[512];
  if (data != NULL && serialize_json(data, encoded, sizeof(encoded)))
    write_success(response, 200U, encoded);
  else write_error(response, 500U, "INTERNAL", "preview metadata could not be encoded");
  if (data != NULL) json_decref(data);
}
static void write_suppressed_get(ls200_gateway *gateway,ls200_gateway_session *session,const ls200_gateway_request *request,ls200_gateway_response *response) { if(strcmp(request->path,"/zoom/api/v1/auth/session")==0) { if(!write_session_response(session,response)) write_error(response,500U,"INTERNAL","session state is invalid"); } else if(strcmp(request->path,"/zoom/api/v1/media/preview")==0) { write_preview_metadata(gateway,response); } else if(strcmp(request->path,"/zoom/api/v1/users")==0) { if(!write_users(gateway,response)) write_error(response,500U,"INTERNAL","account list could not be encoded"); } else if(strcmp(request->path,"/zoom/api/v1/directory")==0) { if(!write_directory(gateway,response)) write_error(response,500U,"INTERNAL","directory could not be encoded"); } else if(strcmp(request->path,"/zoom/api/v1/calls")==0) { if(!write_recents(gateway,response)) write_error(response,500U,"INTERNAL","recent calls could not be encoded"); } else write_error(response,500U,"INTERNAL","local route response is unavailable"); }
static int local_user_response(ls200_gateway *gateway,ls200_gateway_session *session,const ls200_gateway_request *request,const route_plan *plan,ls200_gateway_response *response) { char revoked_username[33]; int result=handle_user_mutation(gateway,session,request,response,revoked_username); if(result==2) { save_mutation(gateway,session,request,plan,response); if(revoked_username[0]!='\0') ls200_gateway_revoke_account_sessions(gateway,revoked_username); } return 1; }
static int local_response(ls200_gateway *gateway,ls200_gateway_session *session,const ls200_gateway_request *request,const route_plan *plan,ls200_gateway_response *response) { if((plan->flags&R_LOGOUT)!=0U) { finish_logout(gateway,session,request,plan,response); return 1; } if((plan->flags&R_SUPPRESS)==0U) return 0; if((plan->flags&R_USERS)!=0U) return local_user_response(gateway,session,request,plan,response); if((plan->flags&R_DIRECTORY)!=0U) { int result=handle_directory_mutation(gateway,request,response); if(result==2) save_mutation(gateway,session,request,plan,response); return 1; } write_suppressed_get(gateway,session,request,response); save_mutation(gateway,session,request,plan,response); return 1; }
static int backend_control_reply(ls200_gateway_control_fn control,void *context,const route_plan *plan,char backend[1024]) { return control==NULL?0:control(context,plan->opcode,plan->control_request_id,plan->payload,backend,1024U); }
static int settings_revision_conflict(const char *backend) {
  static const char *const keys[]={"error","ok"}; static const char *const error_keys[]={"code"};
  json_error_t error; json_t *root,*detail; const char *code; int valid;
  root=json_loads(backend,JSON_REJECT_DUPLICATES,&error); if(root==NULL) return 0;
  detail=json_object_get(root,"error"); code=detail==NULL?NULL:json_string_value(json_object_get(detail,"code"));
  valid=json_object_exact(root,keys,2U)&&json_is_false(json_object_get(root,"ok"))&&json_object_exact(detail,error_keys,1U)&&code!=NULL&&strcmp(code,"REVISION_CONFLICT")==0;
  json_decref(root); return valid;
}
static int backend_persistence_uncertain(const char *backend) {
  static const char *const keys[] = {"error", "ok"};
  static const char *const error_keys[] = {"code"};
  json_error_t error;
  json_t *root;
  json_t *detail;
  const char *code;
  int valid;
  root = json_loads(backend, JSON_REJECT_DUPLICATES, &error);
  if (root == NULL) return 0;
  detail = json_object_get(root, "error");
  code = detail == NULL ? NULL : json_string_value(json_object_get(detail, "code"));
  valid = json_object_exact(root, keys, 2U) && json_is_false(json_object_get(root, "ok")) &&
      json_object_exact(detail, error_keys, 1U) && code != NULL &&
      strcmp(code, "PERSISTENCE_UNCERTAIN") == 0;
  json_decref(root);
  return valid;
}
static int backend_event_response(ls200_gateway *gateway,const route_plan *plan,const char *normalized,ls200_gateway_response *response) { if(plan->opcode!=OPCODE_EVENTS) return 0; if(!record_events(gateway,normalized)||!write_events(gateway,response)) write_error(response,500U,"INTERNAL","event history could not be encoded"); return 1; }
static int backend_recent_call(ls200_gateway *gateway,const ls200_gateway_session *session,const ls200_gateway_request *request,const route_plan *plan,ls200_gateway_response *response) {
  int store_result; if(strcmp(request->path,"/zoom/api/v1/calls")!=0||strcmp(request->method,"POST")!=0) return 1; store_result=record_recent(gateway,request->body);
  if(store_result==1) return 1; if(store_result==2) { write_error(response,503U,"PERSISTENCE_UNCERTAIN","recent call was committed but restart durability was not confirmed"); save_mutation(gateway,session,request,plan,response); return 0; }
  write_error(response,500U,"PERSISTENCE_FAILED","recent call state was not saved"); save_mutation(gateway,session,request,plan,response); return 0;
}
static int backend_status_shape(const ls200_gateway *gateway,const route_plan *plan,const ls200_gateway_request *request,char normalized[1024]) { const char *path=(plan->flags&R_EXPORT)!=0U?"/zoom/api/v1/diagnostics":request->path; return plan->opcode!=OPCODE_STATUS||shape_status_route(gateway,path,normalized,normalized,1024U); }
static int bind_diagnostics_export(ls200_gateway_session *session,const ls200_gateway_request *request,char normalized[1024]) { json_error_t error; json_t *data; if(strcmp(request->path,"/zoom/api/v1/diagnostics")!=0) return 1; if(session->diagnostics_export_id[0]=='\0'||session->diagnostics_export_expires_at<request->now) { uint8_t random[16]; if(RAND_bytes(random,sizeof(random))!=1||!hex_encode(random,sizeof(random),session->diagnostics_export_id,sizeof(session->diagnostics_export_id))) { OPENSSL_cleanse(random,sizeof(random)); return 0; } OPENSSL_cleanse(random,sizeof(random)); } session->diagnostics_export_expires_at=request->now+LS200_GATEWAY_SESSION_IDLE_SECONDS; data=json_loads(normalized,JSON_REJECT_DUPLICATES,&error); if(data==NULL||json_object_set_new(data,"export_id",json_string(session->diagnostics_export_id))!=0||!serialize_json(data,normalized,1024U)) { if(data!=NULL) json_decref(data); return 0; } json_decref(data); return 1; }
static int write_diagnostics_export(ls200_gateway_session *session,const ls200_gateway_request *request,const route_plan *plan,const char *normalized,ls200_gateway_response *response) {
  const char *id; size_t id_length,expected_length; json_error_t error; json_t *data=NULL; char export_data[1400]={0}; int result=0;
  if((plan->flags&R_EXPORT)==0U) goto cleanup;
  id=request->path+strlen("/zoom/api/v1/diagnostics/export/"); id_length=strlen(id); expected_length=strlen(session->diagnostics_export_id);
  if(session->diagnostics_export_expires_at<request->now||id_length!=expected_length||!secure_equal(id,session->diagnostics_export_id,expected_length)) { write_error(response,404U,"EXPORT_NOT_FOUND","diagnostics export is not available"); result=1; goto cleanup; }
  data=json_loads(normalized,JSON_REJECT_DUPLICATES,&error); if(data==NULL||!serialize_json(data,export_data,sizeof(export_data))) { write_error(response,502U,"BACKEND_PROTOCOL","diagnostics export could not be encoded"); result=1; goto cleanup; }
  write_success(response,200U,export_data); result=1;
cleanup:
  if(data!=NULL) json_decref(data); OPENSSL_cleanse(export_data,sizeof(export_data)); return result;
}
static void finish_backend_error(ls200_gateway *gateway,
                                 const ls200_gateway_session *identity,
                                 const ls200_gateway_request *request,
                                 const route_plan *plan,const char *backend,
                                 ls200_gateway_response *response) {
  if(plan->opcode==6U&&backend_persistence_uncertain(backend)) {
    write_error(response,503U,"PERSISTENCE_UNCERTAIN","credential changed but restart durability was not confirmed");
    save_mutation(gateway,identity,request,plan,response);
  } else if(plan->opcode==OPCODE_SETTINGS&&backend_persistence_uncertain(backend)) {
    write_error(response,503U,"PERSISTENCE_UNCERTAIN","settings changed but restart durability was not confirmed");
    save_mutation(gateway,identity,request,plan,response);
  } else if(plan->opcode==OPCODE_SETTINGS&&settings_revision_conflict(backend))
    write_error(response,409U,"REVISION_CONFLICT","settings revision is stale");
  else write_error(response,502U,"BACKEND_PROTOCOL","control daemon rejected the request");
}
static int finish_backend_response(ls200_gateway *gateway,
                                   const ls200_gateway_session *identity,
                                   ls200_gateway_session *live_session,
                                   const ls200_gateway_request *request,
                                   const route_plan *plan,int control_result,
                                   const char *backend,
                                   ls200_gateway_response *response) {
  char normalized[1024]={0};
  if(control_result==0) write_error(response,503U,"BACKEND_UNAVAILABLE","control daemon unavailable");
  else if(control_result==2) finish_backend_error(gateway,identity,request,plan,backend,response);
  else if((plan->flags&R_REDACT)!=0U) { write_success(response,200U,"{\"credentials_updated\":true}"); save_mutation(gateway,identity,request,plan,response); }
  else if(!ls200_gateway_backend_response_normalize(plan->opcode,backend,normalized,sizeof(normalized))) write_error(response,502U,"BACKEND_PROTOCOL","control daemon returned invalid JSON");
  else if(backend_event_response(gateway,plan,normalized,response)) { }
  else if(!backend_recent_call(gateway,identity,request,plan,response)) { }
  else if(!backend_status_shape(gateway,plan,request,normalized)) write_error(response,502U,"BACKEND_PROTOCOL","control daemon status could not be shaped");
  else if(!bind_diagnostics_export(live_session,request,normalized)) write_error(response,500U,"INTERNAL","diagnostics export could not be encoded");
  else if(write_diagnostics_export(live_session,request,plan,normalized,response)) { }
  else { write_success(response,200U,normalized); save_mutation(gateway,identity,request,plan,response); }
  OPENSSL_cleanse(normalized,sizeof(normalized));
  return 1;
}

#include "../device/credentials.h"
enum { R_MUTATION=1U, R_SUPPRESS=2U, R_REDACT=4U, R_UNAVAILABLE=8U, R_BODY=16U, R_LOGOUT=32U, R_USERS=64U, R_DIRECTORY=128U, R_EXPORT=256U, R_DEVICE=512U };
typedef int (*schema_fn)(const char *);
typedef struct { const char *path,*method,*code,*message; uint8_t opcode; ls200_gateway_role role; unsigned flags,status; schema_fn schema; } route_spec;
typedef struct {
  const char *payload;
  uint8_t opcode;
  ls200_gateway_role role;
  unsigned flags;
  int has_request_hash;
  uint32_t control_request_id;
  uint8_t request_hash[LS200_GATEWAY_HASH_BYTES];
} route_plan;

static int safe_control_text(const char *value, size_t minimum, size_t maximum) {
  size_t index, length;
  if (value == NULL) return 0;
  length = strlen(value);
  if (length < minimum || length > maximum || strchr(value, '/') != NULL) return 0;
  for (index = 0U; index < length; ++index) {
    if ((unsigned char)value[index] < 0x21U || (unsigned char)value[index] > 0x7eU ||
        (index + 3U < length && tolower((unsigned char)value[index]) == 's' && tolower((unsigned char)value[index + 1U]) == 'i' && tolower((unsigned char)value[index + 2U]) == 'p' && value[index + 3U] == ':')) return 0;
  }
  return 1;
}
static int digits_only(const char *value, int optional) {
  size_t index;
  if (value == NULL || value[0] == '\0') return optional;
  for (index = 0U; value[index] != '\0'; ++index) if (!isdigit((unsigned char)value[index])) return 0;
  return 1;
}
static int dial_code_valid(const char *value) {
  size_t index;
  if (value == NULL || value[0] == '\0') return 1;
  for (index = 0U; value[index] != '\0'; ++index) if (!isalnum((unsigned char)value[index])) return 0;
  return 1;
}
static int schema_call_request(const char *body) {
  static const char *const keys[]={"dial_code","host_key","layout","meeting_id","passcode","profile"};
  static const char *const layouts[]={"gallery","full_screen","dual_video"};
  static const char *const profiles[]={"zoom_direct","zoom_proxy","private_lab"};
  json_error_t error; json_t *root=parse_json(body,&error); const char *meeting,*profile,*passcode,*host_key,*dial_code; int valid;
  if (root == NULL || !json_object_exact(root,keys,6U)) { if(root!=NULL) json_decref(root); return 0; }
  meeting=json_string_value(json_object_get(root,"meeting_id")); profile=json_string_value(json_object_get(root,"profile")); passcode=json_string_value(json_object_get(root,"passcode")); host_key=json_string_value(json_object_get(root,"host_key")); dial_code=json_string_value(json_object_get(root,"dial_code"));
  valid=safe_control_text(meeting,9U,11U) && digits_only(meeting,0) && safe_control_text(profile,1U,32U) && schema_enum(body,"profile",profiles,3U) && safe_control_text(passcode,0U,64U) && digits_only(passcode,1) && safe_control_text(host_key,0U,10U) && digits_only(host_key,1) && safe_control_text(dial_code,0U,64U) && dial_code_valid(dial_code);
  if(valid) valid=schema_enum(body,"layout",layouts,3U); json_decref(root); return valid;
}
static int schema_settings_request(const char *body) {
  static const char *const keys[]={"media","profile","revision"}; static const char *const legacy_keys[]={"media","profile","revision","tls"}; static const char *const media[]={"managed","disabled"}; static const char *const profiles[]={"zoom_direct","zoom_proxy","private_lab"}; static const char *const tls[]={"required","verified","not_required"};
  json_error_t error; json_t *root=parse_json(body,&error); const char *profile; int has_tls,valid;
  if(root==NULL) return 0;
  has_tls=json_object_get(root,"tls")!=NULL;
  if(!(has_tls?json_object_exact(root,legacy_keys,4U):json_object_exact(root,keys,3U))) { json_decref(root); return 0; }
  profile=json_string_value(json_object_get(root,"profile")); valid=json_is_integer(json_object_get(root,"revision")) && json_integer_value(json_object_get(root,"revision")) >= 1 && (uint64_t)json_integer_value(json_object_get(root,"revision")) <= UINT_MAX && safe_control_text(profile,1U,32U) && schema_enum(body,"profile",profiles,3U) && schema_enum(body,"media",media,2U) && (!has_tls||schema_enum(body,"tls",tls,3U));
  json_decref(root); return valid;
}
static int schema_dtmf_request(const char *body) { static const char *const k[]={"tone"}; static const char *const t[]={"0","1","2","3","4","5","6","7","8","9","*","#","A","B","C","D"}; return schema_keys(body,k,1U) && schema_enum(body,"tone",t,16U); }
static int schema_media_request(const char *body) {
  static const char *const legacy_keys[]={"mode"};
  static const char *const modes[]={"enabled","disabled"};
  static const char *const action_keys[]={"action","value"};
  json_error_t error; json_t *root; const char *action,*value; int valid=0;
  if(schema_keys(body,legacy_keys,1U)) return schema_enum(body,"mode",modes,2U);
  root=parse_json(body,&error);
  if(root==NULL||!json_object_exact(root,action_keys,2U)) { if(root!=NULL) json_decref(root); return 0; }
  action=json_string_value(json_object_get(root,"action"));
  value=json_string_value(json_object_get(root,"value"));
  if(action!=NULL&&value!=NULL) {
    valid=(strcmp(action,"audio_mute")==0&&
        (strcmp(value,"enabled")==0||strcmp(value,"disabled")==0))||
        ((strcmp(action,"keyframe")==0||strcmp(action,"layout_next")==0)&&
         strcmp(value,"request")==0);
  }
  json_decref(root); return valid;
}
static int schema_credentials_request(const char *body) { static const char *const k[]={"password","username"}; return schema_keys(body,k,2U); }
static int schema_diagnostics_request(const char *body) { return body != NULL && strcmp(body,"{}")==0; }

static int key_is_sensitive(const char *key) { static const char *const names[]={"password","secret","token","cookie","authorization"}; size_t index; if(key==NULL) return 1; for(index=0U;index<sizeof(names)/sizeof(names[0]);++index) if(strcasecmp(key,names[index])==0) return 1; return 0; }
static int json_value_is_safe(json_t *value) {
  const char *key; void *iterator; size_t index;
  if(json_is_null(value)||json_is_boolean(value)||json_is_integer(value)||json_is_real(value)) return 1;
  if(json_is_string(value)) return strlen(json_string_value(value))<=256U;
  if(json_is_array(value)) { if(json_array_size(value)>32U) return 0; for(index=0U;index<json_array_size(value);++index) if(!json_value_is_safe(json_array_get(value,index))) return 0; return 1; }
  if(!json_is_object(value)||json_object_size(value)>16U) return 0;
  iterator=json_object_iter(value); while(iterator!=NULL) { key=json_object_iter_key(iterator); if(key_is_sensitive(key)||!json_value_is_safe(json_object_iter_value(iterator))) return 0; iterator=json_object_iter_next(value,iterator); } return 1;
}
static int diagnostics_state_valid(json_t *value, int ready_allowed) {
  const char *state=json_string_value(value);
  return state!=NULL&&(strcmp(state,"not_ready")==0||strcmp(state,"unavailable")==0||
      (ready_allowed&&strcmp(state,"ready")==0));
}
static int diagnostics_check_valid(json_t *check, int ready_allowed) {
  static const char *const keys[]={"state"};
  return json_object_exact(check,keys,1U)&&
      diagnostics_state_valid(json_object_get(check,"state"),ready_allowed);
}
static int diagnostics_backend_valid(json_t *root) {
  static const char *const keys[]={"state","checks"};
  static const char *const check_keys[]={"configuration","sip_control","media_readiness","tls_profile_policy","renderer_truth"};
  json_t *checks;
  if(!json_object_exact(root,keys,2U)||!diagnostics_state_valid(json_object_get(root,"state"),0)) return 0;
  checks=json_object_get(root,"checks");
  return json_object_exact(checks,check_keys,5U)&&
      diagnostics_check_valid(json_object_get(checks,"configuration"),1)&&
      diagnostics_check_valid(json_object_get(checks,"sip_control"),1)&&
      diagnostics_check_valid(json_object_get(checks,"media_readiness"),0)&&
      diagnostics_check_valid(json_object_get(checks,"tls_profile_policy"),1)&&
      diagnostics_check_valid(json_object_get(checks,"renderer_truth"),0);
}
static int event_message_valid(const char *message) {
  static const char *const states[]={"idle","resolving","inviting","early","establishing_media","established","terminating","backing_off","failed","terminated","stopped","terminal_failure"};
  size_t index;
  if(message==NULL) return 0;
  for(index=0U;index<sizeof(states)/sizeof(states[0]);++index) if(strcmp(message,states[index])==0) return 1;
  return 0;
}
static int event_id_valid(const char *id) {
  size_t length,index;
  if(id==NULL||strncmp(id,"call-state-",11U)!=0) return 0;
  length=strlen(id); if(length<12U||length>32U) return 0;
  for(index=11U;index<length;++index) if(!isdigit((unsigned char)id[index])) return 0;
  return 1;
}
static int event_object_valid(json_t *event) {
  static const char *const keys[]={"id","type","message"};
  const char *type,*message;
  if(!json_object_exact(event,keys,3U)) return 0;
  type=json_string_value(json_object_get(event,"type"));
  message=json_string_value(json_object_get(event,"message"));
  return event_id_valid(json_string_value(json_object_get(event,"id")))&&
      type!=NULL&&strcmp(type,"call_state")==0&&event_message_valid(message);
}
static int event_snapshot_backend_valid(json_t *root) {
  static const char *const keys[]={"revision","events"};
  json_t *revision,*events;
  if(!json_object_exact(root,keys,2U)||!json_value_is_safe(root)) return 0;
  revision=json_object_get(root,"revision"); events=json_object_get(root,"events");
  if(!json_is_integer(revision)||json_integer_value(revision)<1||!json_is_array(events)||json_array_size(events)>LS200_GATEWAY_MAX_EVENTS) return 0;
  for (size_t index=0U; index<json_array_size(events); ++index)
    if(!event_object_valid(json_array_get(events,index))) return 0;
  return 1;
}
#include "gateway_metrics.c"

static int operation_backend_valid(json_t *root) { static const char *const keys[]={"ok"}; return json_object_exact(root,keys,1U)&&json_is_boolean(json_object_get(root,"ok"))&&json_is_true(json_object_get(root,"ok")); }
static int serialize_json(json_t *value,char *output,size_t capacity) { char *serialized=json_dumps(value,JSON_COMPACT|JSON_SORT_KEYS); if(serialized==NULL||strlen(serialized)+1U>capacity) { secure_json_free(serialized); return 0; } (void)snprintf(output,capacity,"%s",serialized); secure_json_free(serialized); return 1; }
int ls200_gateway_backend_response_normalize(uint8_t opcode,const char *input,char *output,size_t capacity) {
  json_error_t error; json_t *root; int valid;
  if(input==NULL||output==NULL||capacity==0U||opcode<1U||opcode>OPCODE_METRICS||strlen(input)>LSZ1_MAX_PAYLOAD) return 0;
  root=json_loads(input,JSON_REJECT_DUPLICATES,&error); if(root==NULL) return 0;
  valid=opcode==OPCODE_STATUS?status_backend_valid(root):opcode==7U?
      diagnostics_backend_valid(root):opcode==OPCODE_EVENTS?
      event_snapshot_backend_valid(root):opcode==OPCODE_SETTINGS?
      settings_backend_valid(root):opcode==OPCODE_METRICS?metrics_backend_valid(root):operation_backend_valid(root); if(valid) valid=serialize_json(root,output,capacity); json_decref(root); return valid;
}

static int opaque_export_id(const char *path) { const char *prefix="/zoom/api/v1/diagnostics/export/"; size_t index,length=strlen(prefix); if(strncmp(path,prefix,length)!=0||strlen(path+length)!=32U) return 0; for(index=length;path[index]!='\0';++index) if(!isxdigit((unsigned char)path[index])) return 0; return 1; }
static int passthrough_status_path(const char *path) { return strcmp(path,"/zoom/api/v1/status")==0||strcmp(path,"/zoom/api/v1/calls/active")==0; }
static json_t *collection_status_data(const char *path) {
  static const struct { const char *path,*name; } collection[]={{"/zoom/api/v1/calls","recents"},{"/zoom/api/v1/directory","entries"}}; size_t index;
  for(index=0U;index<sizeof(collection)/sizeof(collection[0]);++index) if(strcmp(path,collection[index].path)==0) return json_pack("{s:[]}",collection[index].name); return NULL;
}
static json_t *route_status_data(const ls200_gateway *gateway,const char *path,json_t *status) {
  json_t *data=collection_status_data(path); if(data!=NULL) return data;
  if(strcmp(path,"/zoom/api/v1/media/preview")==0) return preview_status_data(gateway);
  if(strcmp(path,"/zoom/api/v1/media")==0) return media_status_data(status);
  if(strcmp(path,"/zoom/api/v1/diagnostics")==0) { json_t *last=json_object_get(status,"last_error"); return json_pack("{s:s,s:s,s:s,s:s,s:s,s:O}","last_test","not run","result","live status available","console_version","1","sip_version","1","sbom","installed manifest","last_error",last!=NULL?last:json_null()); }
  return NULL;
}
static int shape_status_route(const ls200_gateway *gateway,const char *path,const char *input,char *output,size_t capacity) {
  json_error_t error; json_t *status,*data; int valid; if(path==NULL||input==NULL||output==NULL||capacity==0U) return 0;
  if(passthrough_status_path(path)) { if(output==input) return 1; if(strlen(input)+1U>capacity) return 0; (void)snprintf(output,capacity,"%s",input); return 1; }
  status=json_loads(input,JSON_REJECT_DUPLICATES,&error); if(!json_is_object(status)) { if(status!=NULL) json_decref(status); return 0; }
  data=route_status_data(gateway,path,status); json_decref(status); if(data==NULL) return 0; valid=serialize_json(data,output,capacity); json_decref(data); return valid;
}

#include "gateway_auth_routes.c"

static const char *role_name(ls200_gateway_role role) {
  return role==LS200_GATEWAY_ROLE_ADMIN?"admin":
      role==LS200_GATEWAY_ROLE_OPERATOR?"operator":
      role==LS200_GATEWAY_ROLE_VIEWER?"viewer":NULL;
}
static int parse_role(const char *value,ls200_gateway_role *role) {
  if(value==NULL||role==NULL) return 0;
  if(strcmp(value,"admin")==0) *role=LS200_GATEWAY_ROLE_ADMIN;
  else if(strcmp(value,"operator")==0) *role=LS200_GATEWAY_ROLE_OPERATOR;
  else if(strcmp(value,"viewer")==0) *role=LS200_GATEWAY_ROLE_VIEWER;
  else return 0;
  return 1;
}
static int account_revision_matches(const ls200_gateway *gateway,json_t *root) {
  json_t *revision=json_object_get(root,"revision");
  return json_is_integer(revision)&&json_integer_value(revision)>0&&
      (uint64_t)json_integer_value(revision)<=INT_MAX&&
      (unsigned int)json_integer_value(revision)==gateway->account_revision;
}
static int directory_id(const char *value) {
  size_t index;
  if(value==NULL||value[0]=='\0'||strlen(value)>32U) return 0;
  for(index=0U;value[index]!='\0';++index)
    if((unsigned char)value[index]<0x21U||(unsigned char)value[index]>0x7eU||
       (!isalnum((unsigned char)value[index])&&value[index]!='_'&&value[index]!='-')) return 0;
  return 1;
}
static int directory_name_character_is_safe(unsigned char character) {
  return character >= 0x20U && character <= 0x7eU && character != '/' &&
      character != '@' && character != '%' && character != '\\';
}
static int directory_name_has_sip_scheme(const char *value, size_t index,
                                         size_t length) {
  return index + 3U < length && tolower((unsigned char)value[index]) == 's' &&
      tolower((unsigned char)value[index + 1U]) == 'i' &&
      tolower((unsigned char)value[index + 2U]) == 'p' && value[index + 3U] == ':';
}
static int directory_name(const char *value) {
  size_t index,length;
  if(value==NULL||value[0]=='\0'||strlen(value)>64U) return 0;
  length=strlen(value);
  for(index=0U;value[index]!='\0';++index) {
    if(!directory_name_character_is_safe((unsigned char)value[index])||
       directory_name_has_sip_scheme(value,index,length)) return 0;
  }
  return 1;
}
static int directory_profile(const char *value) { return value!=NULL&&(strcmp(value,"zoom_direct")==0||strcmp(value,"zoom_proxy")==0||strcmp(value,"private_lab")==0); }
static int directory_layout(const char *value) { return value!=NULL&&(strcmp(value,"gallery")==0||strcmp(value,"full_screen")==0||strcmp(value,"dual_video")==0); }
static int directory_meeting(const char *value) { return value!=NULL&&strlen(value)>=9U&&strlen(value)<=11U&&digits_only(value,0); }
static ls200_gateway_safe_reference *find_reference(ls200_gateway_safe_reference *entries,const char *id) { size_t i; for(i=0U;i<LS200_GATEWAY_MAX_DIRECTORY_ENTRIES;++i) if(entries[i].used&&strcmp(entries[i].id,id)==0) return &entries[i]; return NULL; }
static int serialize_references(const ls200_gateway_safe_reference *entries,size_t count,unsigned int revision,const char *name,char output[LS200_GATEWAY_COLLECTION_RESPONSE_BYTES]) {
  json_t *items=json_array(),*data; size_t i; int valid;
  if(items==NULL) return 0;
  for(i=0U;i<count;++i) { json_t *item; int appended; if(!entries[i].used) continue; item=json_pack("{s:s,s:s,s:s,s:s,s:s}","id",entries[i].id,"name",entries[i].name,"meeting_id",entries[i].meeting_id,"profile",entries[i].profile,"default_layout",entries[i].default_layout); if(item==NULL) { json_decref(items); return 0; } appended=json_array_append(items,item); json_decref(item); if(appended!=0) { json_decref(items); return 0; } }
  data=json_pack("{s:i,s:O}","revision",(int)revision,name,items); json_decref(items); if(data==NULL) return 0;
  valid=serialize_json(data,output,LS200_GATEWAY_COLLECTION_RESPONSE_BYTES); json_decref(data); return valid;
}
static int write_directory(const ls200_gateway *gateway,ls200_gateway_response *response) { char data[LS200_GATEWAY_COLLECTION_RESPONSE_BYTES]; if(!serialize_references(gateway->directory,LS200_GATEWAY_MAX_DIRECTORY_ENTRIES,gateway->directory_revision,"entries",data)) return 0; write_success(response,200U,data); return 1; }
static int write_recents(const ls200_gateway *gateway,ls200_gateway_response *response) { char data[LS200_GATEWAY_COLLECTION_RESPONSE_BYTES]; if(!serialize_references(gateway->recents,LS200_GATEWAY_MAX_RECENTS,1U,"recents",data)) return 0; write_success(response,200U,data); return 1; }
static int directory_mutation_schema(json_t *root,int deleting,ls200_gateway_safe_reference *value) {
  const char *id,*name,*meeting,*profile,*layout;
  if(!json_is_object(root)||json_object_size(root)!=(deleting?2U:6U)) return 0;
  id=json_string_value(json_object_get(root,"id")); if(!directory_id(id)||json_object_get(root,"revision")==NULL) return 0;
  if(deleting) { (void)snprintf(value->id,sizeof(value->id),"%s",id); return 1; }
  name=json_string_value(json_object_get(root,"name")); meeting=json_string_value(json_object_get(root,"meeting_id")); profile=json_string_value(json_object_get(root,"profile")); layout=json_string_value(json_object_get(root,"default_layout"));
  if(!directory_name(name)||!directory_meeting(meeting)||!directory_profile(profile)||!directory_layout(layout)) return 0;
  value->used=1; (void)snprintf(value->id,sizeof(value->id),"%s",id); (void)snprintf(value->name,sizeof(value->name),"%s",name); (void)snprintf(value->meeting_id,sizeof(value->meeting_id),"%s",meeting); (void)snprintf(value->profile,sizeof(value->profile),"%s",profile); (void)snprintf(value->default_layout,sizeof(value->default_layout),"%s",layout); return 1;
}
static int directory_revision_matches(const ls200_gateway *gateway,json_t *root) {
  json_t *revision=json_object_get(root,"revision");
  return json_is_integer(revision)&&
      json_integer_value(revision)==(json_int_t)gateway->directory_revision;
}
static ls200_gateway_safe_reference *first_free_reference(ls200_gateway *gateway) {
  size_t index;
  for(index=0U;index<LS200_GATEWAY_MAX_DIRECTORY_ENTRIES;++index)
    if(!gateway->directory[index].used) return &gateway->directory[index];
  return NULL;
}
static int directory_mutation_target(ls200_gateway *gateway,int deleting,
                                     ls200_gateway_safe_reference *existing,
                                     ls200_gateway_safe_reference **target,
                                     ls200_gateway_response *response) {
  if(deleting&&existing==NULL) {
    write_error(response,404U,"DIRECTORY_NOT_FOUND","entry is not configured");
    return 0;
  }
  *target=existing;
  if(!deleting&&*target==NULL) *target=first_free_reference(gateway);
  if(!deleting&&*target==NULL) {
    write_error(response,409U,"DIRECTORY_CAPACITY","directory capacity is reached");
    return 0;
  }
  return 1;
}
static int handle_directory_mutation(ls200_gateway *gateway,const ls200_gateway_request *request,ls200_gateway_response *response) {
  json_error_t error; json_t *root=parse_json(request->body,&error); ls200_gateway_safe_reference value,*existing,*target; ls200_gateway_safe_reference backup[LS200_GATEWAY_MAX_DIRECTORY_ENTRIES]; unsigned int backup_revision=0U; ls200_gateway_store_result store_result=LS200_GATEWAY_STORE_DURABLE; int deleting=strcmp(request->method,"DELETE")==0; char data[160];
  (void)memset(&value,0,sizeof(value)); if(root==NULL||!directory_mutation_schema(root,deleting,&value)) { if(root!=NULL) json_decref(root); write_error(response,400U,"SCHEMA_INVALID","body schema is invalid"); return 1; }
  if(!directory_revision_matches(gateway,root)) { json_decref(root); write_error(response,409U,"REVISION_CONFLICT","directory revision is stale"); return 1; }
  json_decref(root); existing=find_reference(gateway->directory,value.id);
  if(!directory_mutation_target(gateway,deleting,existing,&target,response)) return 1;
  if(gateway->directory_revision==(unsigned int)INT_MAX) { write_error(response,409U,"REVISION_CONFLICT","directory revision cannot advance"); return 1; }
  (void)memcpy(backup,gateway->directory,sizeof(backup)); backup_revision=gateway->directory_revision; if(deleting) OPENSSL_cleanse(existing,sizeof(*existing)); else *target=value; ++gateway->directory_revision;
  if(gateway->config.account_store_path!=NULL) store_result=ls200_gateway_store_account(gateway);
  if(store_result==LS200_GATEWAY_STORE_NOT_COMMITTED) { (void)memcpy(gateway->directory,backup,sizeof(backup)); gateway->directory_revision=backup_revision; OPENSSL_cleanse(backup,sizeof(backup)); write_error(response,500U,"PERSISTENCE_FAILED","directory state was not saved"); return 1; }
  OPENSSL_cleanse(backup,sizeof(backup));
  if(store_result==LS200_GATEWAY_STORE_DURABILITY_UNCERTAIN) { write_error(response,503U,"PERSISTENCE_UNCERTAIN","directory was committed but restart durability was not confirmed"); return 2; }
  (void)snprintf(data,sizeof(data),"{\"id\":\"%s\",\"revision\":%u}",value.id,gateway->directory_revision); write_success(response,deleting?200U:(existing==NULL?201U:200U),data); return 2;
}
static int record_recent(ls200_gateway *gateway,const char *body) {
  json_error_t error; json_t *root=json_loads(body,JSON_REJECT_DUPLICATES,&error); ls200_gateway_safe_reference value; uint8_t random[16]; ls200_gateway_safe_reference backup[LS200_GATEWAY_MAX_RECENTS]; ls200_gateway_store_result store_result=LS200_GATEWAY_STORE_DURABLE;
  if(root==NULL) return 0; (void)memset(&value,0,sizeof(value));
  if(!directory_meeting(json_string_value(json_object_get(root,"meeting_id")))||!directory_profile(json_string_value(json_object_get(root,"profile")))||!directory_layout(json_string_value(json_object_get(root,"layout")) )||RAND_bytes(random,sizeof(random))!=1||!hex_encode(random,sizeof(random),value.id,sizeof(value.id))) { json_decref(root); OPENSSL_cleanse(random,sizeof(random)); return 0; }
  (void)snprintf(value.name,sizeof(value.name),"Meeting %s",json_string_value(json_object_get(root,"meeting_id"))); (void)snprintf(value.meeting_id,sizeof(value.meeting_id),"%s",json_string_value(json_object_get(root,"meeting_id"))); (void)snprintf(value.profile,sizeof(value.profile),"%s",json_string_value(json_object_get(root,"profile"))); (void)snprintf(value.default_layout,sizeof(value.default_layout),"%s",json_string_value(json_object_get(root,"layout"))); value.used=1; json_decref(root); OPENSSL_cleanse(random,sizeof(random));
  (void)memcpy(backup,gateway->recents,sizeof(backup)); (void)memmove(&gateway->recents[1],&gateway->recents[0],sizeof(gateway->recents)-sizeof(gateway->recents[0])); gateway->recents[0]=value;
  if(gateway->config.account_store_path!=NULL) store_result=ls200_gateway_store_account(gateway);
  if(store_result==LS200_GATEWAY_STORE_NOT_COMMITTED) { (void)memcpy(gateway->recents,backup,sizeof(backup)); OPENSSL_cleanse(backup,sizeof(backup)); return 0; }
  OPENSSL_cleanse(backup,sizeof(backup)); return store_result==LS200_GATEWAY_STORE_DURABILITY_UNCERTAIN?2:1;
}
static int event_already_recorded(const ls200_gateway *gateway,const char *id,
                                  const char *type,const char *message) {
  size_t slot;
  for(slot=0U;slot<LS200_GATEWAY_MAX_EVENTS;++slot) {
    if(!gateway->events[slot].used||strcmp(gateway->events[slot].id,id)!=0) continue;
    return strcmp(gateway->events[slot].type,type)==0&&
        strcmp(gateway->events[slot].message,message)==0?1:-1;
  }
  return 0;
}
static int record_events(ls200_gateway *gateway,const char *body) {
  json_error_t error; json_t *root=json_loads(body,JSON_REJECT_DUPLICATES,&error),*events; size_t i;
  if(root==NULL) return 0; events=json_object_get(root,"events");
  for(i=0U;i<json_array_size(events);++i) { json_t *item=json_array_get(events,i); const char *id=json_string_value(json_object_get(item,"id")); const char *type=json_string_value(json_object_get(item,"type")); const char *message=json_string_value(json_object_get(item,"message")); int recorded=event_already_recorded(gateway,id,type,message); if(recorded<0) { json_decref(root); return 0; } if(recorded>0) continue; (void)memmove(&gateway->events[1],&gateway->events[0],sizeof(gateway->events)-sizeof(gateway->events[0])); (void)memset(&gateway->events[0],0,sizeof(gateway->events[0])); gateway->events[0].used=1; (void)snprintf(gateway->events[0].id,sizeof(gateway->events[0].id),"%s",id); (void)snprintf(gateway->events[0].type,sizeof(gateway->events[0].type),"%s",type); (void)snprintf(gateway->events[0].message,sizeof(gateway->events[0].message),"%s",message); }
  json_decref(root); return 1;
}
static int write_events(const ls200_gateway *gateway,ls200_gateway_response *response) {
  json_t *events=json_array(),*data; char serialized[LS200_GATEWAY_COLLECTION_RESPONSE_BYTES]; size_t i; if(events==NULL) return 0;
  for(i=0U;i<LS200_GATEWAY_MAX_EVENTS;++i) { json_t *item; int appended; if(!gateway->events[i].used) continue; item=json_pack("{s:s,s:s,s:s}","id",gateway->events[i].id,"type",gateway->events[i].type,"message",gateway->events[i].message); if(item==NULL) { json_decref(events); return 0; } appended=json_array_append(events,item); json_decref(item); if(appended!=0) { json_decref(events); return 0; } }
  data=json_pack("{s:O}","events",events); json_decref(events); if(data==NULL||!serialize_json(data,serialized,sizeof(serialized))) { if(data!=NULL) json_decref(data); return 0; } json_decref(data); write_success(response,200U,serialized); return 1;
}
static int write_users(ls200_gateway *gateway,ls200_gateway_response *response) {
  json_t *users=json_array(),*data;
  size_t index;
  char serialized[1024];
  if(users==NULL) return 0;
  for(index=0U;index<LS200_GATEWAY_MAX_ACCOUNTS;++index) {
    const ls200_gateway_account *account=&gateway->accounts[index];
    json_t *entry;
    if(!account->configured) continue;
    entry=json_pack("{s:s,s:s}","username",account->username,"role",role_name(account->role));
    if(entry==NULL) { json_decref(users); return 0; }
    { int appended=json_array_append(users,entry); json_decref(entry); if(appended!=0) { json_decref(users); return 0; } }
  }
  data=json_pack("{s:i,s:O}","revision",(int)gateway->account_revision,"users",users);
  json_decref(users);
  if(data==NULL||!serialize_json(data,serialized,sizeof(serialized))) { if(data!=NULL) json_decref(data); return 0; }
  json_decref(data); write_success(response,200U,serialized); return 1;
}
static int user_mutation_schema(json_t *root,int deleting,const char **username,
                                const char **password,ls200_gateway_role *role) {
  *username=json_string_value(json_object_get(root,"username"));
  if(!json_is_object(root)||json_object_size(root)!=(deleting?2U:4U)||
      json_object_get(root,"username")==NULL||json_object_get(root,"revision")==NULL||
      (!deleting&&(json_object_get(root,"password")==NULL||json_object_get(root,"role")==NULL))||
      !is_safe_username(*username)) return 0;
  if(deleting) return 1;
  *password=json_string_value(json_object_get(root,"password"));
  return *password!=NULL&&strlen(*password)<=256U&&
      parse_role(json_string_value(json_object_get(root,"role")),role);
}
static int user_mutation_precondition(const ls200_gateway *gateway,
                                      const ls200_gateway_account *existing,
                                      int deleting,ls200_gateway_role role,
                                      ls200_gateway_response *response) {
  if(deleting&&existing==NULL) {
    write_error(response,404U,"USER_NOT_FOUND","account is not configured");
    return 0;
  }
  if(!deleting&&existing==NULL&&
     ls200_gateway_account_count(gateway)>=LS200_GATEWAY_MAX_ACCOUNTS) {
    write_error(response,409U,"ACCOUNT_CAPACITY","account capacity is reached");
    return 0;
  }
  if(existing!=NULL&&existing->role==LS200_GATEWAY_ROLE_ADMIN&&
     (deleting||role!=LS200_GATEWAY_ROLE_ADMIN)&&
     ls200_gateway_admin_count(gateway)==1U) {
    write_error(response,409U,"LAST_ADMIN","at least one administrator is required");
    return 0;
  }
  return 1;
}
static int apply_user_mutation(ls200_gateway *gateway,int deleting,
                               const char *username,const char *password,
                               ls200_gateway_role role) {
  if(deleting) return ls200_gateway_delete_account(gateway,username);
  return ls200_gateway_upsert_account(gateway,username,password,role);
}
static int handle_user_mutation(ls200_gateway *gateway,ls200_gateway_session *session,
                                const ls200_gateway_request *request,
                                ls200_gateway_response *response,char revoked_username[33]) {
  json_error_t error;
  json_t *root=parse_json(request->body,&error);
  const char *username=NULL,*password=NULL;
  const ls200_gateway_account *existing;
  ls200_gateway_role role=LS200_GATEWAY_ROLE_VIEWER;
  int deleting=strcmp(request->method,"DELETE")==0;
  int had_existing;
  ls200_gateway_store_result store_result;
  ls200_gateway_account backup_accounts[LS200_GATEWAY_MAX_ACCOUNTS];
  ls200_gateway_account backup_legacy;
  unsigned int backup_revision=0U;
  char data[128],target_username[33];
  revoked_username[0]='\0';
  if(root==NULL||!user_mutation_schema(root,deleting,&username,&password,&role)) {
    if(root!=NULL) json_decref(root); write_error(response,400U,"SCHEMA_INVALID","body schema is invalid"); return 1;
  }
  (void)snprintf(target_username,sizeof(target_username),"%s",username);
  if(!account_revision_matches(gateway,root)) { json_decref(root); write_error(response,409U,"REVISION_CONFLICT","account revision is stale"); return 1; }
  existing=ls200_gateway_find_account(gateway,target_username);
  had_existing=existing!=NULL;
  if(!user_mutation_precondition(gateway,existing,deleting,role,response)) { json_decref(root); return 1; }
  if(gateway->account_revision==(unsigned int)INT_MAX) { json_decref(root); write_error(response,409U,"REVISION_CONFLICT","account revision cannot advance"); return 1; }
  (void)memcpy(backup_accounts,gateway->accounts,sizeof(backup_accounts));
  backup_legacy=gateway->account;
  backup_revision=gateway->account_revision;
  if(!apply_user_mutation(gateway,deleting,target_username,password,role)) {
    (void)memcpy(gateway->accounts,backup_accounts,sizeof(backup_accounts)); gateway->account=backup_legacy; gateway->account_revision=backup_revision; json_decref(root); OPENSSL_cleanse(backup_accounts,sizeof(backup_accounts)); OPENSSL_cleanse(&backup_legacy,sizeof(backup_legacy)); write_error(response,400U,"SCHEMA_INVALID","body schema is invalid"); return 1;
  }
  ++gateway->account_revision;
  store_result=ls200_gateway_store_account(gateway);
  if(store_result==LS200_GATEWAY_STORE_NOT_COMMITTED) { (void)memcpy(gateway->accounts,backup_accounts,sizeof(backup_accounts)); gateway->account=backup_legacy; gateway->account_revision=backup_revision; json_decref(root); OPENSSL_cleanse(backup_accounts,sizeof(backup_accounts)); OPENSSL_cleanse(&backup_legacy,sizeof(backup_legacy)); write_error(response,500U,"PERSISTENCE_FAILED","account state was not saved"); return 1; }
  json_decref(root);
  OPENSSL_cleanse(backup_accounts,sizeof(backup_accounts));
  OPENSSL_cleanse(&backup_legacy,sizeof(backup_legacy));
  if(had_existing) (void)snprintf(revoked_username,33U,"%s",target_username);
  if(store_result==LS200_GATEWAY_STORE_DURABILITY_UNCERTAIN) { write_error(response,503U,"PERSISTENCE_UNCERTAIN","account was committed but restart durability was not confirmed"); return 2; }
  (void)snprintf(data,sizeof(data),"{\"username\":\"%s\",\"role\":\"%s\",\"revision\":%u}",target_username,deleting?"deleted":role_name(role),gateway->account_revision);
  write_success(response,deleting?200U:(existing==NULL?201U:200U),data);
  (void)session;
  return 2;
}

static int schema_oem_credentials(const char *body) {
  json_error_t error;
  json_t *arguments = parse_json(body, &error);
  int valid = ls200_device_credentials_schema(arguments);
  json_decref(arguments);
  return valid;
}

static const route_spec ROUTES[]={
  {"/zoom/api/v1/device/credentials","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_ADMIN,R_DEVICE,0U,NULL},
  {"/zoom/api/v1/device/credentials","PUT","SCHEMA_INVALID","credential schema is invalid",0U,LS200_GATEWAY_ROLE_ADMIN,R_DEVICE|R_MUTATION|R_BODY|R_REDACT,400U,schema_oem_credentials},
  {"/zoom/api/v1/device/status","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_VIEWER,R_DEVICE,0U,NULL},
  {"/zoom/api/v1/library","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_VIEWER,R_UNAVAILABLE,0U,NULL},
  {"/zoom/api/v1/schedule","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_VIEWER,R_UNAVAILABLE,0U,NULL},
  {"/zoom/api/v1/device/settings","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_ADMIN,R_UNAVAILABLE,0U,NULL},
  {"/zoom/api/v1/maintenance","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_ADMIN,R_UNAVAILABLE,0U,NULL},

  {"/zoom/api/v1/auth/logout","POST",NULL,NULL,0U,LS200_GATEWAY_ROLE_VIEWER,R_MUTATION|R_SUPPRESS|R_LOGOUT,0U,NULL},{"/zoom/api/v1/auth/session","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_VIEWER,R_SUPPRESS,0U,NULL},{"/zoom/api/v1/status","GET",NULL,NULL,OPCODE_STATUS,LS200_GATEWAY_ROLE_VIEWER,0U,0U,NULL},{"/zoom/api/v1/events","GET",NULL,NULL,OPCODE_EVENTS,LS200_GATEWAY_ROLE_VIEWER,0U,0U,NULL},{"/zoom/api/v1/calls","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_VIEWER,R_SUPPRESS,0U,NULL},
  {"/zoom/api/v1/calls","POST","SCHEMA_INVALID","body schema is invalid",2U,LS200_GATEWAY_ROLE_OPERATOR,R_MUTATION|R_BODY,400U,schema_call_request},{"/zoom/api/v1/calls/active","GET",NULL,NULL,OPCODE_STATUS,LS200_GATEWAY_ROLE_VIEWER,0U,0U,NULL},{"/zoom/api/v1/calls/active","DELETE",NULL,NULL,OPCODE_HANGUP,LS200_GATEWAY_ROLE_OPERATOR,R_MUTATION,0U,NULL},{"/zoom/api/v1/calls/active/dtmf","POST","SCHEMA_INVALID","body schema is invalid",4U,LS200_GATEWAY_ROLE_OPERATOR,R_MUTATION|R_BODY,400U,schema_dtmf_request},{"/zoom/api/v1/calls/active/media","PATCH","SCHEMA_INVALID","body schema is invalid",OPCODE_MEDIA,LS200_GATEWAY_ROLE_OPERATOR,R_MUTATION|R_BODY,400U,schema_media_request},
  {"/zoom/api/v1/directory","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_VIEWER,R_SUPPRESS,0U,NULL},{"/zoom/api/v1/directory","POST",NULL,NULL,0U,LS200_GATEWAY_ROLE_ADMIN,R_MUTATION|R_SUPPRESS|R_DIRECTORY|R_BODY,0U,NULL},{"/zoom/api/v1/directory","DELETE",NULL,NULL,0U,LS200_GATEWAY_ROLE_ADMIN,R_MUTATION|R_SUPPRESS|R_DIRECTORY|R_BODY,0U,NULL},{"/zoom/api/v1/media","GET",NULL,NULL,OPCODE_STATUS,LS200_GATEWAY_ROLE_VIEWER,0U,0U,NULL},{"/zoom/api/v1/media/preview","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_VIEWER,R_SUPPRESS,0U,NULL},{"/zoom/api/v1/settings","GET",NULL,NULL,OPCODE_SETTINGS,LS200_GATEWAY_ROLE_ADMIN,0U,0U,NULL},{"/zoom/api/v1/settings","PATCH","SCHEMA_INVALID","settings schema is invalid",OPCODE_SETTINGS,LS200_GATEWAY_ROLE_ADMIN,R_MUTATION|R_BODY,400U,schema_settings_request},{"/zoom/api/v1/settings/credentials","POST","SCHEMA_INVALID","body schema is invalid",6U,LS200_GATEWAY_ROLE_ADMIN,R_MUTATION|R_REDACT|R_BODY,400U,schema_credentials_request},
  {"/zoom/api/v1/users","GET",NULL,NULL,0U,LS200_GATEWAY_ROLE_ADMIN,R_SUPPRESS,0U,NULL},{"/zoom/api/v1/users","POST",NULL,NULL,0U,LS200_GATEWAY_ROLE_ADMIN,R_MUTATION|R_SUPPRESS|R_USERS|R_BODY,0U,NULL},{"/zoom/api/v1/users","DELETE",NULL,NULL,0U,LS200_GATEWAY_ROLE_ADMIN,R_MUTATION|R_SUPPRESS|R_USERS|R_BODY,0U,NULL},{"/zoom/api/v1/diagnostics","GET",NULL,NULL,OPCODE_STATUS,LS200_GATEWAY_ROLE_ADMIN,0U,0U,NULL},{"/zoom/api/v1/diagnostics/metrics","GET",NULL,NULL,OPCODE_METRICS,LS200_GATEWAY_ROLE_ADMIN,0U,0U,NULL},{"/zoom/api/v1/diagnostics/tests","POST","SCHEMA_INVALID","body must be exactly {}",7U,LS200_GATEWAY_ROLE_ADMIN,R_MUTATION,400U,schema_diagnostics_request}
};
static int dispatch_route(const ls200_gateway_request *request,ls200_gateway_response *response,route_plan *plan) {
  size_t index; const route_spec *route=NULL;
  for(index=0U;index<sizeof(ROUTES)/sizeof(ROUTES[0]);++index) if(strcmp(request->path,ROUTES[index].path)==0&&strcmp(request->method,ROUTES[index].method)==0) { route=&ROUTES[index]; break; }
  if(route==NULL&&opaque_export_id(request->path)&&strcmp(request->method,"GET")==0) { plan->role=LS200_GATEWAY_ROLE_ADMIN; plan->opcode=OPCODE_STATUS; plan->flags=R_EXPORT; return 1; }
  if(route==NULL) return 0; if(route->schema!=NULL&&!route->schema(request->body)) { write_error(response,route->status,route->code,route->message); return -1; }
  plan->opcode=route->opcode; plan->role=route->role; plan->flags=route->flags; if((route->flags&R_BODY)!=0U) plan->payload=request->body; return 1;
}
static void save_mutation(ls200_gateway *gateway,const ls200_gateway_session *session,const ls200_gateway_request *request,const route_plan *plan,const ls200_gateway_response *response) { if((plan->flags&R_MUTATION)!=0U&&plan->has_request_hash) save_idempotency(gateway,session,request->idempotency_key,request->path,plan->request_hash,response); }
static int operation_request_id(const ls200_gateway_session *session,const ls200_gateway_request *request,uint32_t *output) {
  EVP_MD_CTX *context=EVP_MD_CTX_new(); unsigned int length=0U; uint8_t digest[LS200_GATEWAY_HASH_BYTES]; uint32_t value=0U; int valid;
  if(context==NULL||session==NULL||request==NULL||request->path==NULL||request->idempotency_key==NULL||output==NULL) { EVP_MD_CTX_free(context); return 0; }
  valid=EVP_DigestInit_ex(context,EVP_sha256(),NULL)==1&&EVP_DigestUpdate(context,session->session_id,sizeof(session->session_id))==1&&EVP_DigestUpdate(context,request->path,strlen(request->path)+1U)==1&&EVP_DigestUpdate(context,request->idempotency_key,strlen(request->idempotency_key))==1&&EVP_DigestFinal_ex(context,digest,&length)==1&&length==sizeof(digest);
  EVP_MD_CTX_free(context); if(!valid) { OPENSSL_cleanse(digest,sizeof(digest)); return 0; }
  (void)memcpy(&value,digest,sizeof(value)); OPENSSL_cleanse(digest,sizeof(digest)); *output=value==0U?1U:value; return 1;
}
static int preflight(ls200_gateway *gateway,ls200_gateway_session *session,const ls200_gateway_request *request,route_plan *plan,ls200_gateway_response *response) {
  ls200_gateway_idempotency *cached; if((plan->flags&R_MUTATION)!=0U&&!is_safe_idempotency_key(request->idempotency_key)) { write_error(response,400U,"IDEMPOTENCY_REQUIRED","a valid idempotency key is required"); return 1; }
  if((plan->flags&R_MUTATION)!=0U&&!idempotency_request_hash(request,plan->request_hash)) { write_error(response,(plan->flags&R_USERS)!=0U?400U:500U,(plan->flags&R_USERS)!=0U?"SCHEMA_INVALID":"INTERNAL",(plan->flags&R_USERS)!=0U?"body schema is invalid":"request fingerprint failed"); return 1; }
  if((plan->flags&R_MUTATION)!=0U) plan->has_request_hash=1;
  if((plan->flags&R_MUTATION)!=0U&&!operation_request_id(session,request,&plan->control_request_id)) { write_error(response,500U,"INTERNAL","operation identifier failed"); return 1; }
  if((plan->flags&R_MUTATION)!=0U&&(cached=find_idempotency(gateway,session,request->idempotency_key,request->path))!=NULL) {
    if(!secure_equal(cached->request_hash,plan->request_hash,sizeof(cached->request_hash))) { write_error(response,409U,"IDEMPOTENCY_CONFLICT","idempotency key was already used for a different request"); return 1; }
    response->status=cached->status; (void)snprintf(response->content_type,sizeof(response->content_type),"application/json"); (void)snprintf(response->body,sizeof(response->body),"%s",cached->response); return 1;
  }
  if((plan->flags&R_UNAVAILABLE)==0U) return 0; write_error(response,501U,"CAPABILITY_UNAVAILABLE","this operation is not enabled in the current device build"); save_mutation(gateway,session,request,plan,response); return 1;
}
#include "gateway_route_response.c"
#include "gateway_route_request.c"

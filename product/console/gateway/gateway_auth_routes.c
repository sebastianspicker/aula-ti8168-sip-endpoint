static int login_schema_valid(json_t *root,const char *username,const char *password) { static const char *const keys[]={"username","password"}; return json_object_exact(root,keys,2U)&&is_safe_username(username)&&password!=NULL&&strlen(password)<=256U; }
typedef struct login_hash_snapshot {
  ls200_gateway_account account;
  unsigned int revision;
} login_hash_snapshot;

static void snapshot_login_account(const ls200_gateway *gateway,const char *username,
                                   login_hash_snapshot *snapshot) {
  static const uint8_t salt_dummy[LS200_GATEWAY_SALT_BYTES]={0x6c,0x73,0x32,0x30,0x30,0x2d,0x67,0x61,0x74,0x65,0x77,0x61,0x79,0x2d,0x76,0x31};
  static const uint8_t hash_dummy[LS200_GATEWAY_HASH_BYTES]={0x85,0xfb,0x07,0xfe,0x0d,0x66,0xb1,0x66,0xb1,0xd0,0x7a,0x1b,0xe9,0xf5,0xd4,0xd1,0xe2,0xef,0xa7,0xdb,0xce,0x5d,0xf2,0xbc,0x73,0xf1,0x82,0xa9,0x61,0x39,0xa6,0x56};
  const ls200_gateway_account *account=ls200_gateway_find_account(gateway,username);
  (void)memset(snapshot,0,sizeof(*snapshot));
  snapshot->revision=gateway->account_revision;
  if(account!=NULL) snapshot->account=*account;
  else {
    (void)memcpy(snapshot->account.salt,salt_dummy,sizeof(snapshot->account.salt));
    (void)memcpy(snapshot->account.password_hash,hash_dummy,
                 sizeof(snapshot->account.password_hash));
  }
}

static int login_account_unchanged(const ls200_gateway *gateway,const char *username,
                                   const login_hash_snapshot *snapshot) {
  const ls200_gateway_account *account=ls200_gateway_find_account(gateway,username);
  return snapshot->account.configured&&gateway->account_revision==snapshot->revision&&
      account!=NULL&&account->configured==snapshot->account.configured&&
      account->role==snapshot->account.role&&
      strcmp(account->username,snapshot->account.username)==0&&
      secure_equal(account->salt,snapshot->account.salt,sizeof(account->salt))&&
      secure_equal(account->password_hash,snapshot->account.password_hash,
                   sizeof(account->password_hash));
}

static int reserve_login_hash(ls200_gateway *gateway,const char *username,
                              const ls200_gateway_request *request,
                              login_hash_snapshot *snapshot,
                              ls200_gateway_response *response) {
  unsigned int retry=0U;
  int reserved=0;
  if(!gateway_state_lock(gateway)) {
    write_error(response,500U,"INTERNAL","authentication state is unavailable");
    return 0;
  }
  if(!auth_budget_take(gateway,username,request,&retry))
    write_rate_limited(response,retry);
  else if(gateway->login_hash_active)
    write_rate_limited(response,1U);
  else {
    snapshot_login_account(gateway,username,snapshot);
    gateway->login_hash_active=1;
    reserved=1;
  }
  gateway_state_unlock(gateway);
  return reserved;
}

static int handle_login(ls200_gateway *gateway,const ls200_gateway_request *request,ls200_gateway_response *response) {
  json_error_t error; json_t *root=NULL; const char *username,*password; char authenticated_username[33]={0},csrf[65]={0},data[128]={0}; uint8_t candidate[LS200_GATEWAY_HASH_BYTES]={0}; login_hash_snapshot snapshot; int derived=0;
  (void)memset(&snapshot,0,sizeof(snapshot));
  if(!request_origin_is_allowed(gateway,request)) { write_error(response,403U,"ORIGIN_REQUIRED","exact Origin is required"); goto cleanup; }
  root=parse_json(request->body,&error); if(root==NULL) { write_error(response,400U,"BAD_JSON","body must be JSON"); goto cleanup; }
  username=json_string_value(json_object_get(root,"username")); password=json_string_value(json_object_get(root,"password"));
  if(!login_schema_valid(root,username,password)) { write_error(response,400U,"SCHEMA_INVALID","body schema is invalid"); goto cleanup; }
  (void)snprintf(authenticated_username,sizeof(authenticated_username),"%s",username);
  if(!reserve_login_hash(gateway,username,request,&snapshot,response)) goto cleanup;
  derived=password_hash_candidate(password,snapshot.account.salt,candidate);
  if(!gateway_state_lock(gateway)) {
    write_error(response,500U,"INTERNAL","authentication state is unavailable");
    goto cleanup;
  }
  gateway->login_hash_active=0;
  if(!derived||!secure_equal(candidate,snapshot.account.password_hash,
      sizeof(candidate))||!login_account_unchanged(gateway,authenticated_username,
      &snapshot)) {
    write_error(response,401U,"AUTH_INVALID","invalid credentials");
  } else if(new_session(gateway,authenticated_username,snapshot.account.role,
      request->now,response->set_cookie,csrf)==NULL) {
    write_error(response,500U,"INTERNAL","session creation failed");
  } else {
    (void)snprintf(data,sizeof(data),"{\"csrf_token\":\"%s\",\"role\":%u}",
                   csrf,(unsigned)snapshot.account.role);
    write_success(response,200U,data);
  }
  gateway_state_unlock(gateway);
cleanup:
  if(root!=NULL) json_decref(root);
  OPENSSL_cleanse(data,sizeof(data));
  OPENSSL_cleanse(candidate,sizeof(candidate));
  OPENSSL_cleanse(&snapshot,sizeof(snapshot));
  OPENSSL_cleanse(authenticated_username,sizeof(authenticated_username));
  OPENSSL_cleanse(csrf,sizeof(csrf));
  return 1;
}
static int bootstrap_schema_valid(json_t *root,const char *username,const char *password,const char *code) { static const char *const keys[]={"username","password","bootstrap_code"}; return json_object_exact(root,keys,3U)&&is_safe_username(username)&&password!=NULL&&code!=NULL; }
static int bootstrap_code_matches(const ls200_gateway *gateway,const char *code) { return strlen(code)==strlen(gateway->config.bootstrap_code)&&secure_equal(code,gateway->config.bootstrap_code,strlen(gateway->config.bootstrap_code)); }
static int handle_bootstrap(ls200_gateway *gateway,const ls200_gateway_request *request,ls200_gateway_response *response) {
  json_error_t error; json_t *root=NULL; const char *username,*password,*code; uint8_t salt[LS200_GATEWAY_SALT_BYTES]={0}; unsigned retry=0U; ls200_gateway_account backup_accounts[LS200_GATEWAY_MAX_ACCOUNTS]={{0}},backup_account={0}; unsigned int backup_revision=0U; int backup_disabled=0; ls200_gateway_store_result store_result; int result=1;
  if(!request_origin_is_allowed(gateway,request)) { write_error(response,403U,"ORIGIN_REQUIRED","exact Origin is required"); goto cleanup; }
  if(gateway->bootstrap_disabled||ls200_gateway_account_count(gateway)!=0U||gateway->config.bootstrap_code==NULL) { write_error(response,409U,"BOOTSTRAP_DISABLED","bootstrap is disabled"); goto cleanup; }
  root=parse_json(request->body,&error); if(root==NULL) { write_error(response,400U,"BAD_JSON","body must be JSON"); goto cleanup; }
  username=json_string_value(json_object_get(root,"username")); password=json_string_value(json_object_get(root,"password")); code=json_string_value(json_object_get(root,"bootstrap_code"));
  if(!bootstrap_schema_valid(root,username,password,code)) { write_error(response,400U,"SCHEMA_INVALID","body schema is invalid"); goto cleanup; }
  if(!auth_budget_take(gateway,username,request,&retry)) { write_rate_limited(response,retry); goto cleanup; }
  (void)memcpy(backup_accounts,gateway->accounts,sizeof(backup_accounts)); backup_account=gateway->account; backup_revision=gateway->account_revision; backup_disabled=gateway->bootstrap_disabled;
  if(!bootstrap_code_matches(gateway,code)||RAND_bytes(salt,sizeof(salt))!=1||!ls200_gateway_set_account(gateway,username,password,LS200_GATEWAY_ROLE_ADMIN,salt)) { (void)memcpy(gateway->accounts,backup_accounts,sizeof(backup_accounts)); gateway->account=backup_account; gateway->account_revision=backup_revision; gateway->bootstrap_disabled=backup_disabled; write_error(response,403U,"BOOTSTRAP_INVALID","bootstrap request rejected"); goto cleanup; }
  gateway->bootstrap_disabled=1;
  store_result=ls200_gateway_store_account(gateway);
  if(store_result==LS200_GATEWAY_STORE_NOT_COMMITTED) { (void)memcpy(gateway->accounts,backup_accounts,sizeof(backup_accounts)); gateway->account=backup_account; gateway->account_revision=backup_revision; gateway->bootstrap_disabled=backup_disabled; write_error(response,500U,"PERSISTENCE_FAILED","account state was not saved"); goto cleanup; }
  disable_bootstrap_code(gateway);
  if(store_result==LS200_GATEWAY_STORE_DURABILITY_UNCERTAIN) { write_error(response,503U,"PERSISTENCE_UNCERTAIN","account was committed but restart durability was not confirmed"); goto cleanup; }
  write_success(response,201U,"{\"bootstrapped\":true}");
cleanup:
  if(root!=NULL) json_decref(root);
  OPENSSL_cleanse(salt,sizeof(salt));
  OPENSSL_cleanse(backup_accounts,sizeof(backup_accounts));
  OPENSSL_cleanse(&backup_account,sizeof(backup_account));
  return result;
}
static int dispatch_auth(ls200_gateway *gateway,const ls200_gateway_request *request,ls200_gateway_response *response) {
  if(strcmp(request->path,"/zoom/api/v1/auth/bootstrap")==0) { if(strcmp(request->method,"POST")!=0) write_error(response,405U,"METHOD_NOT_ALLOWED","POST is required"); else (void)handle_bootstrap(gateway,request,response); return 1; }
  return 0;
}

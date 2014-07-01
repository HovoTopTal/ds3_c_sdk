/*
 * ******************************************************************************
 *   Copyright 2014 Spectra Logic Corporation. All Rights Reserved.
 *   Licensed under the Apache License, Version 2.0 (the "License"). You may not use
 *   this file except in compliance with the License. A copy of the License is located at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 *   or in the "license" file accompanying this file.
 *   This file is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 *   CONDITIONS OF ANY KIND, either express or implied. See the License for the
 *   specific language governing permissions and limitations under the License.
 * ****************************************************************************
 */

#include <glib.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/xmlmemory.h>

#include "ds3.h"

//---------- Define opaque struct ----------//  
struct _ds3_request{
    http_verb verb;
    char* path;
    size_t path_size;
    uint64_t length;
    GHashTable* headers;
    GHashTable* query_params;

    //These next few elements are only for the bulk commands
    ds3_bulk_object_list* object_list;
};

typedef struct {
    char* buff;
    size_t size;
    size_t total_read;
}ds3_xml_send_buff;

typedef struct {
    uint64_t status_code;
    char* status_message;
    size_t status_message_size;
    size_t header_count;
    GHashTable* headers;
}ds3_response_data;

typedef struct {
    char* key;
    size_t key_size;
    char* value;
    size_t value_size;
}ds3_response_header;

static void _ds3_free_response_header(gpointer data) {
    ds3_response_header* header;
    if (data == NULL) {
        return;
    }
        
    header = (ds3_response_header*) data;
    g_free(header->key);
    g_free(header->value);
    g_free(data);
    
}

static ds3_error* _ds3_create_error(ds3_error_code code, const char * message) {
    ds3_error* error = g_new0(ds3_error, 1);
    error->code = code;
    error->message = g_strdup(message);
    error->message_size = strlen(error->message);
    return error;
}

static size_t _ds3_send_xml_buff(void* buffer, size_t size, size_t nmemb, void* user_data) {
    size_t to_read; 
    size_t remaining;
    ds3_xml_send_buff* xml_buff;
    
    xml_buff = (ds3_xml_send_buff*) user_data;
    to_read = size * nmemb;
    remaining = xml_buff->size - xml_buff->total_read;

    if(remaining < to_read) {
        to_read = remaining;
    }
    
    strncpy((char*)buffer, xml_buff->buff, to_read);
    return to_read;
}

static size_t _process_header_line(void* buffer, size_t size, size_t nmemb, void* user_data) {
    size_t to_read;
    char* header_buff;
    char** split_result;
    ds3_response_header* header;
    ds3_response_data* response_data = (ds3_response_data*) user_data;
    GHashTable* headers = response_data->headers;
    
    printf("size %d, nmemb %d\n", size, nmemb); 
    to_read = size * nmemb;
    printf("Header ToRead: %d\n", to_read);
    if (to_read < 2) {
        return 0;
    }
    
    header_buff = g_new0(char, to_read+1); //+1 for the null byte
    strncpy(header_buff, (char*)buffer, to_read);
    header_buff = g_strchomp(header_buff);
    
    // If we have read all the headers, then the last line will only be \n\r
    if (strlen(header_buff) == 0) {
        g_free(header_buff);
        return to_read;
    }

    printf("Header value: %s\n", header_buff);
    printf("Current Header Count: %lu\n", response_data->header_count);
    
    if (response_data->header_count < 1) {
        printf("Processing the status line, is this changing\n");
        if (g_str_has_prefix(header_buff, "HTTP/1.1") == TRUE) {
            // parse out status code and the status string
            char* endpointer;
            uint64_t status_code;
            split_result = g_strsplit(header_buff, " ", 1000);
            status_code = g_ascii_strtoll(split_result[1], &endpointer, 10);
            if (status_code == 0 && endpointer != NULL) {
                fprintf(stderr, "Encountered a problem parsing the status code\n");
                g_strfreev(split_result);
                g_free(header_buff);
                return 0;
            }
            if (status_code == 100) {
                printf("Ignoring 100 status code header line\n");
                g_free(header_buff);
                return to_read;
            }
            else {
                response_data->status_code = status_code;
                response_data->status_message = g_strjoinv(" ", split_result + 2);
                printf("Status Message: %s\n", response_data->status_message);
                response_data->status_message_size = strlen(response_data->status_message);
            }
            g_strfreev(split_result);
        }
        else {
            fprintf(stderr, "Unsupported Protocol\n");
            g_free(header_buff);
            return 0;
        }
    }
    else {
        printf("Processing a header\n");
        split_result = g_strsplit(header_buff, ": ", 2);
        header = g_new0(ds3_response_header, 1); 
        header->key = g_strdup(split_result[0]);
        header->key_size = strlen(header->key);
        header->value = g_strdup(split_result[1]);
        header->value_size = strlen(header->value);

        g_hash_table_insert(headers, header->key, header);
        
        g_strfreev(split_result);
    }
    response_data->header_count++;
    g_free(header_buff);
    return to_read;
}

//---------- Networking code ----------// 
static void _init_curl(void) {
    static ds3_bool initialized = False;

    if(!initialized) {
        if(curl_global_init(CURL_GLOBAL_ALL) != 0) {
          fprintf(stderr, "Encountered an error initializing libcurl\n");
        }
        initialized = True;
    }
}

static char* _net_get_verb(http_verb verb) {
    switch(verb) {
        case HTTP_GET: return "GET";
        case HTTP_PUT: return "PUT";
        case HTTP_POST: return "POST";
        case HTTP_DELETE : return "DELETE";
        case HTTP_HEAD : return "HEAD";
    }

    return NULL;
}

static unsigned char* _generate_signature_str(http_verb verb, char* resource_name, char* date,
                               char* content_type, char* md5, char* amz_headers) {
    char* verb_str; 
    if(resource_name == NULL) {
        fprintf(stderr, "resource_name is required\n");
        return NULL;
    }
    if(date == NULL) {
        fprintf(stderr, "date is required");
        return NULL;
    }
    verb_str = _net_get_verb(verb);

    return (unsigned char*) g_strconcat(verb_str, "\n", md5, "\n", content_type, "\n", date, "\n", amz_headers, resource_name, NULL);
}

static char* _generate_date_string(void) {
    GDateTime* time  = g_date_time_new_now_local();
    char* date_string = g_date_time_format(time, "%a, %d %b %Y %T %z");
    
    fprintf(stdout, "Date: %s\n", date_string);
    g_date_time_unref(time);

    return date_string;
}

static char* _net_compute_signature(const ds3_creds* creds, http_verb verb, char* resource_name,
                             char* date, char* content_type, char* md5, char* amz_headers) {
    GHmac* hmac;
    gchar* signature;
    gsize bufSize = 256;
    guint8 buffer[256];
    unsigned char* signature_str = _generate_signature_str(verb, resource_name, date, content_type, md5, amz_headers); 
    
    fprintf(stdout, "Signature:\n%s\n", signature_str);
  

    hmac = g_hmac_new(G_CHECKSUM_SHA1, (unsigned char*) creds->secret_key, creds->secret_key_len);
    g_hmac_update(hmac, signature_str, -1);
    g_hmac_get_digest(hmac, buffer, &bufSize);
    
    signature = g_base64_encode(buffer, bufSize);
    
    g_free(signature_str);
    g_hmac_unref(hmac);

    return signature;
}

typedef struct {
    char** entries;
    size_t size;
}query_entries;

static void _hash_for_each(gpointer _key, gpointer _value, gpointer _user_data) {
    char* key = (char*) _key;
    char* value = (char*) _value;
    query_entries* entries = (query_entries*) _user_data;

    entries->entries[entries->size] = g_strconcat(key, "=", value, NULL);
}

static char* _net_gen_query_params(GHashTable* query_params) {
    if (g_hash_table_size(query_params) > 0) {
        query_entries q_entries;
        char** entries;
        char* return_string;
        int i;
        //build the query string
        memset(&q_entries, 0, sizeof(query_entries));

        //We need the +1 so that it is NULL terminating for g_strjoinv
        entries = g_new0(char*, g_hash_table_size(query_params) + 1);
        q_entries.entries = entries;
        g_hash_table_foreach(query_params, _hash_for_each, &q_entries);
        
        return_string = g_strjoinv("&", entries);

        for(i= 0; ; i++ ) {
            char* current_string = entries[i];
            if(current_string == NULL) {
                break;
            }
            g_free(current_string);
        }

        g_free(entries);
        return return_string;
    }
    else {
        return NULL;
    }
}

static ds3_error* _net_process_request(const ds3_client* client, const ds3_request* _request, void* read_user_struct, size_t (*read_handler_func)(void*, size_t, size_t, void*), void* write_user_struct, size_t (*write_handler_func)(void*, size_t, size_t, void*)) {
    _init_curl();
    
    struct _ds3_request* request = (struct _ds3_request*) _request;
    CURL* handle = curl_easy_init();
    CURLcode res;

    if(handle) {
        char* url;
        
        char* date;
        char* date_header;
        char* signature;
        struct curl_slist* headers;
        char* auth_header;
        char* query_params = _net_gen_query_params(request->query_params);
        ds3_response_data response_data;
        GHashTable* response_headers = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, _ds3_free_response_header);

        memset(&response_data, 0, sizeof(ds3_response_data));
        
        response_data.headers = response_headers;

        if (query_params == NULL) {
            url = g_strconcat(client->endpoint, request->path, NULL);
        }
        else {
            url = g_strconcat(client->endpoint, request->path,"?",query_params, NULL);
            g_free(query_params);
        }
        curl_easy_setopt(handle, CURLOPT_URL, url);
        curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L); //tell curl to follow redirects
        curl_easy_setopt(handle, CURLOPT_MAXREDIRS, client->num_redirects);
        
        // Setup header collection
        curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, _process_header_line);
        curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response_data);

        if(client->proxy != NULL) {
          curl_easy_setopt(handle, CURLOPT_PROXY, client->proxy);
        }

        // Register the read and write handlers if they are set
        if(read_user_struct != NULL && read_handler_func != NULL) {
            curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, read_handler_func);
            curl_easy_setopt(handle, CURLOPT_WRITEDATA, read_user_struct);
        }

        if(write_user_struct != NULL && write_handler_func != NULL) {
            curl_easy_setopt(handle, CURLOPT_READFUNCTION, write_handler_func);
            curl_easy_setopt(handle, CURLOPT_READDATA, write_user_struct);
        }

        switch(request->verb) {
            case HTTP_POST: {
                if (write_user_struct == NULL || write_handler_func == NULL) {
                    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "POST");
                }
                else {
                    curl_easy_setopt(handle, CURLOPT_POST, 1L);
                    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
                    curl_easy_setopt(handle, CURLOPT_INFILESIZE, request->length);
                }
                break;
            }
            case HTTP_PUT: {
                if (write_user_struct == NULL || write_handler_func == NULL) {
                    curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
                }
                else {
                    curl_easy_setopt(handle, CURLOPT_PUT, 1L);
                    curl_easy_setopt(handle, CURLOPT_UPLOAD, 1L);
                    curl_easy_setopt(handle, CURLOPT_INFILESIZE, request->length);
                }
                break;
            }
            case HTTP_DELETE: {
                curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
                break;
            }
            case HTTP_HEAD: {
                curl_easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "HEAD");
                break;
            }
            case HTTP_GET: {
                //Placeholder if we need to put anything here.
                break;
            }
        }

        date = _generate_date_string(); 
        date_header = g_strconcat("Date: ", date, NULL);
        signature = _net_compute_signature(client->creds, request->verb, request->path, date, "", "", "");
        headers = NULL;
        auth_header = g_strconcat("Authorization: AWS ", client->creds->access_id, ":", signature, NULL);

        headers = curl_slist_append(headers, auth_header);
        headers = curl_slist_append(headers, date_header);

        curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);

        res = curl_easy_perform(handle);

        g_free(url);
        g_free(date);
        g_free(date_header);
        g_free(signature);
        g_free(auth_header);
        g_free(response_data.status_message); 
        g_hash_table_destroy(response_headers);
        curl_slist_free_all(headers);
        curl_easy_cleanup(handle);
        if(res != CURLE_OK) {
            char * message = g_strconcat("Request failed: ", curl_easy_strerror(res), NULL);
            ds3_error* error = _ds3_create_error(DS3_ERROR_FAILED_REQUEST, message);
            g_free(message);
            return error;
        }
    }
    else {
        return _ds3_create_error(DS3_ERROR_CURL_HANDLE, "Failed to create curl handle");
    }
    return NULL;
}

static void net_cleanup(void) {
    curl_global_cleanup();
}

//---------- Ds3 code ----------// 
static GHashTable* _create_hash_table(void) {
    GHashTable* hash =  g_hash_table_new(g_str_hash, g_str_equal);
    return hash;
}

ds3_creds* ds3_create_creds(const char* access_id, const char* secret_key) {
    ds3_creds* creds;
    if(access_id == NULL || secret_key == NULL) {
        fprintf(stderr, "Arguments cannot be NULL\n");
        return NULL;
    }
    
    creds = g_new0(ds3_creds, 1);

    creds->access_id = g_strdup(access_id);
    creds->access_id_len = strlen(creds->access_id);

    creds->secret_key = g_strdup(secret_key); 
    creds->secret_key_len = strlen(creds->secret_key);

    return creds;
}

ds3_client* ds3_create_client(const char* endpoint, ds3_creds* creds) {
    ds3_client* client;
    if(endpoint == NULL) {
        fprintf(stderr, "Null endpoint\n");
        return NULL;
    }

    client = g_new0(ds3_client, 1);
    
    client->endpoint = g_strdup(endpoint);
    client->endpoint_len = strlen(endpoint);
    
    client->creds = creds;

    client->num_redirects = 5L; //default to 5 redirects before failing
    return client;
}

void ds3_client_proxy(ds3_client* client, const char* proxy) {
    client->proxy = g_strdup(proxy);
    client->proxy_len = strlen(proxy);
}

static struct _ds3_request* _common_request_init(void){
    struct _ds3_request* request = g_new0(struct _ds3_request, 1);
    request->headers = _create_hash_table();
    request->query_params = _create_hash_table();
    return request;
}

ds3_request* ds3_init_get_service(void) {
    struct _ds3_request* request = _common_request_init(); 
    request->verb = HTTP_GET;
    request->path =  g_new0(char, 2);
    request->path [0] = '/';
    return (ds3_request*) request;
}

ds3_request* ds3_init_get_bucket(const char* bucket_name) {
    struct _ds3_request* request = _common_request_init(); 
    request->verb = HTTP_GET;
    request->path = g_strconcat("/", bucket_name, NULL);
    return (ds3_request*) request;
}

ds3_request* ds3_init_get_object(const char* bucket_name, const char* object_name) {
    struct _ds3_request* request = _common_request_init();
    request->verb = HTTP_GET;
    request->path = g_strconcat("/", bucket_name, "/", object_name, NULL);
    return (ds3_request*) request;
}

ds3_request* ds3_init_delete_object(const char* bucket_name, const char* object_name) {
    struct _ds3_request* request = _common_request_init();
    request->verb = HTTP_DELETE;
    request->path = g_strconcat("/", bucket_name, "/", object_name, NULL);
    return (ds3_request*) request;
}

ds3_request* ds3_init_put_object(const char* bucket_name, const char* object_name, uint64_t length) {
    struct _ds3_request* request = _common_request_init();
    request->verb = HTTP_PUT;
    request->path = g_strconcat("/", bucket_name, "/", object_name, NULL);
    request->length = length;
    return (ds3_request*) request;
}

ds3_request* ds3_init_put_bucket(const char* bucket_name) {
    struct _ds3_request* request = _common_request_init();
    request->verb = HTTP_PUT;
    request->path = g_strconcat("/", bucket_name, NULL);
    return (ds3_request*) request;
}

ds3_request* ds3_init_delete_bucket(const char* bucket_name) {
    struct _ds3_request* request = _common_request_init();
    request->verb = HTTP_DELETE;
    request->path = g_strconcat("/", bucket_name, NULL);
    return (ds3_request*) request;
}

ds3_request* ds3_init_get_bulk(const char* bucket_name, ds3_bulk_object_list* object_list) {
    struct _ds3_request* request = _common_request_init();
    request->verb = HTTP_PUT;
    request->path = g_strconcat("/_rest_/bucket/", bucket_name, NULL);
    g_hash_table_insert(request->query_params, "operation", "start_bulk_get");
    request->object_list = object_list;
    return (ds3_request*) request;
}

ds3_request* ds3_init_put_bulk(const char* bucket_name, ds3_bulk_object_list* object_list) {
    struct _ds3_request* request = _common_request_init();
    request->verb = HTTP_PUT;
    request->path = g_strconcat("/_rest_/bucket/", bucket_name, NULL);
    g_hash_table_insert(request->query_params, "operation", "start_bulk_put");
    request->object_list = object_list;
    return (ds3_request*) request;
}

static ds3_error* _internal_request_dispatcher(const ds3_client* client, const ds3_request* request, void* read_user_struct, size_t (*read_handler_func)(void*, size_t, size_t, void*), void* write_user_struct, size_t (*write_handler_func)(void*, size_t, size_t, void*)) {
    if(client == NULL || request == NULL) {
        return _ds3_create_error(DS3_ERROR_MISSING_ARGS, "All arguments must be filled in for request processing"); 
    }
    return _net_process_request(client, request, read_user_struct, read_handler_func, write_user_struct, write_handler_func);
}

static size_t load_xml_buff(void* contents, size_t size, size_t nmemb, void* user_data) {
    size_t realsize = size * nmemb;
    GByteArray* blob = (GByteArray*) user_data;
    
    g_byte_array_append(blob, (const guint8 *) contents, realsize);
    return realsize;
}

static void _parse_buckets(xmlDocPtr doc, xmlNodePtr buckets_node, ds3_get_service_response* response) {
    xmlChar* text;
    xmlNodePtr data_ptr; 
    xmlNodePtr curr;
    GArray* array = g_array_new(FALSE, TRUE, sizeof(ds3_bucket));

    for(curr = buckets_node->xmlChildrenNode; curr != NULL; curr = curr->next) {
        ds3_bucket bucket; 
        memset(&bucket, 0, sizeof(ds3_bucket));
        
        for(data_ptr = curr->xmlChildrenNode; data_ptr != NULL; data_ptr = data_ptr->next) {
            if(xmlStrcmp(data_ptr->name, (const xmlChar*) "CreationDate") == 0) {
                text = xmlNodeListGetString(doc, data_ptr->xmlChildrenNode, 1);
                bucket.creation_date = g_strdup((char*) text);
                bucket.creation_date_size = strlen((char*) text);
                xmlFree(text);
            }
            else if(xmlStrcmp(data_ptr->name, (const xmlChar*) "Name") == 0) {
                text = xmlNodeListGetString(doc, data_ptr->xmlChildrenNode, 1);
                bucket.name = g_strdup((char*) text);
                bucket.name_size = strlen((char*) text);
                xmlFree(text);
            }
            else {
                fprintf(stderr, "Unknown element: (%s)\n", data_ptr->name);
            }
        }
        g_array_append_val(array, bucket);
    }

    response->num_buckets = array->len;
    response->buckets = (ds3_bucket*)array->data;
    g_array_free(array, FALSE);
}

static ds3_owner* _parse_owner(xmlDocPtr doc, xmlNodePtr owner_node) {
    xmlNodePtr child_node;
    xmlChar* text;
    ds3_owner* owner = g_new0(ds3_owner, 1);

    for(child_node = owner_node->xmlChildrenNode; child_node != NULL; child_node = child_node->next) {
        if(xmlStrcmp(child_node->name, (const xmlChar*) "DisplayName") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            owner->name = g_strdup((char*) text);
            owner->name_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "ID") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            owner->id = g_strdup((char*) text);
            owner->id_size = strlen((char*) text);
            xmlFree(text);
        }
        else {
            fprintf(stderr, "Unknown xml element: (%s)\n", child_node->name);
        }
    }

    return owner;
}

ds3_error* ds3_get_service(const ds3_client* client, const ds3_request* request, ds3_get_service_response** _response) {
    ds3_get_service_response* response;
    xmlDocPtr doc;
    xmlNodePtr root;
    xmlNodePtr child_node;
    GByteArray* xml_blob = g_byte_array_new();
    
    _internal_request_dispatcher(client, request, xml_blob, load_xml_buff, NULL, NULL);
   
    doc = xmlParseMemory((const char*) xml_blob->data, xml_blob->len);

    if(doc == NULL) {
        char* message = g_strconcat("Failed to parse response document.  The actual response is: ", xml_blob->data, NULL);
        g_byte_array_free(xml_blob, TRUE);
        ds3_error* error = _ds3_create_error(DS3_ERROR_INVALID_XML, message);
        g_free(message);
        return error;
    }

    root = xmlDocGetRootElement(doc);
    
    if(xmlStrcmp(root->name, (const xmlChar*) "ListAllMyBucketsResult") != 0) {
        char* message = g_strconcat("Expected the root element to be 'ListAllMyBucketsResult'.  The actual response is: ", xml_blob->data, NULL);
        xmlFreeDoc(doc);
        g_byte_array_free(xml_blob, TRUE);
        ds3_error* error = _ds3_create_error(DS3_ERROR_INVALID_XML, message);
        g_free(message);
        return error;
    }

    response = g_new0(ds3_get_service_response, 1);
    
    for(child_node = root->xmlChildrenNode; child_node != NULL; child_node = child_node->next) {
        if(xmlStrcmp(child_node->name, (const xmlChar*) "Buckets") == 0) {
            //process buckets here
            _parse_buckets(doc, child_node, response);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "Owner") == 0) {
            //process owner here
            ds3_owner * owner = _parse_owner(doc, child_node);
            response->owner = owner;
        }
        else {
            fprintf(stderr, "Unknown xml element: (%s)\b", child_node->name);
        }
    }

    xmlFreeDoc(doc);
    g_byte_array_free(xml_blob, TRUE);
    *_response = response;
    return NULL;
}

static ds3_object _parse_object(xmlDocPtr doc, xmlNodePtr contents_node) {
    xmlNodePtr child_node;
    xmlChar* text;
    ds3_object object;
    memset(&object, 0, sizeof(ds3_object));

    for(child_node = contents_node->xmlChildrenNode; child_node != NULL; child_node = child_node->next) {
        if(xmlStrcmp(child_node->name, (const xmlChar*) "Key") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            object.name = g_strdup((char*) text);
            object.name_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "ETag") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            object.etag= g_strdup((char*) text);
            object.etag_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "LastModified") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            object.last_modified = g_strdup((char*) text);
            object.last_modified_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "StorageClass") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            object.storage_class = g_strdup((char*) text);
            object.storage_class_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "Size") == 0) { 
            uint64_t size; 
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            size = strtoul((const char*)text, NULL, 10);
            object.size = size;
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "Owner") == 0) { 
            ds3_owner* owner = _parse_owner(doc, child_node);
            object.owner = owner;
        }        
        else {
            fprintf(stderr, "Unknown xml element: (%s)\n", child_node->name);
        }
    }

    return object;
}

ds3_error* ds3_get_bucket(const ds3_client* client, const ds3_request* request, ds3_get_bucket_response** _response) {
    ds3_get_bucket_response* response;
    xmlDocPtr doc;
    xmlNodePtr root;
    xmlNodePtr child_node;
    xmlChar* text;
    GArray* object_array = g_array_new(FALSE, TRUE, sizeof(ds3_object));
    GByteArray* xml_blob = g_byte_array_new();
    _internal_request_dispatcher(client, request, xml_blob, load_xml_buff, NULL, NULL);
    
    doc = xmlParseMemory((const char*) xml_blob->data, xml_blob->len);
    if(doc == NULL) {
        char* message = g_strconcat("Failed to parse response document.  The actual response is: ", xml_blob->data, NULL);
        g_byte_array_free(xml_blob, TRUE);
        ds3_error* error = _ds3_create_error(DS3_ERROR_INVALID_XML, message);
        g_free(message);
        return error;
    }

    root = xmlDocGetRootElement(doc);

    if(xmlStrcmp(root->name, (const xmlChar*) "ListBucketResult") != 0) {
        char* message = g_strconcat("Expected the root element to be 'ListBucketsResult'.  The actual response is: ", xml_blob->data, NULL);
        g_byte_array_free(xml_blob, TRUE);
        xmlFreeDoc(doc);
        ds3_error* error = _ds3_create_error(DS3_ERROR_INVALID_XML, message);
        g_free(message);
        return error;
    }

    response = g_new0(ds3_get_bucket_response, 1);

    for(child_node = root->xmlChildrenNode; child_node != NULL; child_node = child_node->next) {
        if(xmlStrcmp(child_node->name, (const xmlChar*) "Contents") == 0) {
            ds3_object object = _parse_object(doc, child_node);
            g_array_append_val(object_array, object);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "CreationDate") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            response->creation_date = g_strdup((char*) text);
            response->creation_date_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "IsTruncated") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            if(strncmp((char*) text, "true", 4) == 0) {
                response->is_truncated = True; 
            }
            else {
                response->is_truncated = False; 
            }
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "Marker") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            response->marker = g_strdup((char*) text);
            response->marker_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "MaxKeys") == 0) {
            uint64_t max_keys; 
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            max_keys = strtoul((const char*)text, NULL, 10); 
            response->max_keys = max_keys;
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "Name") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            response->name = g_strdup((char*) text);
            response->name_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "Delimiter") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            response->delimiter= g_strdup((char*) text);
            response->delimiter_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "NextMarker") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            response->next_marker= g_strdup((char*) text);
            response->next_marker_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(child_node->name, (const xmlChar*) "Prefix") == 0) {
            text = xmlNodeListGetString(doc, child_node->xmlChildrenNode, 1);
            if(text == NULL) {
                continue;
            }
            response->prefix = g_strdup((char*) text);
            response->prefix_size = strlen((char*) text);
            xmlFree(text);
        }
        else {
            fprintf(stderr, "Unknown element: (%s)\n", child_node->name);
        }
    }

    response->objects = (ds3_object*) object_array->data;
    response->num_objects = object_array->len;
    xmlFreeDoc(doc);
    g_array_free(object_array, FALSE);
    g_byte_array_free(xml_blob, TRUE);
    *_response = response;
    return NULL;
}

ds3_error* ds3_get_object(const ds3_client* client, const ds3_request* request, void* user_data, size_t(*callback)(void*,size_t, size_t, void*)) {
    return _internal_request_dispatcher(client, request, user_data, callback, NULL, NULL);
}

ds3_error* ds3_put_object(const ds3_client* client, const ds3_request* request, void* user_data, size_t (*callback)(void*, size_t, size_t, void*)) {
    return _internal_request_dispatcher(client, request, NULL, NULL, user_data, callback);
}

ds3_error* ds3_delete_object(const ds3_client* client, const ds3_request* request) {
    return _internal_request_dispatcher(client, request, NULL, NULL, NULL, NULL);
}

ds3_error* ds3_put_bucket(const ds3_client* client, const ds3_request* request) {
    return _internal_request_dispatcher(client, request, NULL, NULL, NULL, NULL);
}

ds3_error* ds3_delete_bucket(const ds3_client* client, const ds3_request* request) {
    return _internal_request_dispatcher(client, request, NULL, NULL, NULL, NULL);
}

static ds3_bulk_object _parse_bulk_object(xmlDocPtr doc, xmlNodePtr object_node) {
    xmlNodePtr child_node;
    xmlChar* text;
    struct _xmlAttr* attribute;

    ds3_bulk_object response;
    memset(&response, 0, sizeof(ds3_bulk_object));

    for(attribute = object_node->properties; attribute != NULL; attribute = attribute->next) {
        if(xmlStrcmp(attribute->name, (const xmlChar*) "Name") == 0) {
            text = xmlNodeListGetString(doc, attribute->children, 1);
            if(text == NULL) {
                continue;
            }
            response.name = g_strdup((char*) text);
            response.name_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(attribute->name, (const xmlChar*) "Size") == 0) {
            uint64_t size;
            text = xmlNodeListGetString(doc, attribute->children, 1);
            if(text == NULL) {
                continue;
            }
            size = strtoul((const char*)text, NULL, 10);
            response.size = size;
            xmlFree(text);
        }
        else {
            fprintf(stderr, "Unknown attribute: (%s)\n", attribute->name);
        }
    }

    for(child_node = object_node->xmlChildrenNode; child_node != NULL; child_node = child_node->next) {
        fprintf(stderr, "Unknown element: (%s)\n", child_node->name);
    }

    return response;
}

static ds3_bulk_object_list* _parse_bulk_objects(xmlDocPtr doc, xmlNodePtr objects_node) {
    xmlNodePtr object_node, child_node;
    xmlChar* text;
    struct _xmlAttr* attribute;

    ds3_bulk_object_list* response = g_new0(ds3_bulk_object_list, 1);
    GArray* object_array = g_array_new(FALSE, TRUE, sizeof(ds3_bulk_object));

    for(attribute = objects_node->properties; attribute != NULL; attribute = attribute->next) {
        if(xmlStrcmp(attribute->name, (const xmlChar*) "ServerId") == 0) {
            text = xmlNodeListGetString(doc, attribute->children, 1);
            if(text == NULL) {
                continue;
            }
            response->server_id= g_strdup((char*) text);
            response->server_id_size = strlen((char*) text);
            xmlFree(text);
        }
        else if(xmlStrcmp(attribute->name, (const xmlChar*) "ChunkNumber") == 0) {
            uint64_t chunk_number;
            text = xmlNodeListGetString(doc, attribute->children, 1);
            if(text == NULL) {
                continue;
            }
            chunk_number = strtoul((const char*)text, NULL, 10);
            response->chunk_number = chunk_number;
            xmlFree(text);
        }
        else {
            fprintf(stderr, "Unknown attribute: (%s)\n", attribute->name);
        }

    }

    for(child_node = objects_node->xmlChildrenNode; child_node != NULL; child_node = child_node->next) {
        if(xmlStrcmp(child_node->name, (const xmlChar*) "Object") == 0) {
            ds3_bulk_object object = _parse_bulk_object(doc, child_node);
            g_array_append_val(object_array, object);
        }
        else {
            fprintf(stderr, "Unknown element: (%s)\n", child_node->name);
        }
    }

    response->list = (ds3_bulk_object*) object_array->data;
    response->size = object_array->len;
    g_array_free(object_array, FALSE);
    return response;
}

ds3_error* ds3_bulk(const ds3_client* client, const ds3_request* _request, ds3_bulk_response** _response) {
    ds3_bulk_response* response;
    ds3_error* error_response;
    xmlChar* text;
    xmlNodePtr root, root_node, objects_node, object_node, child_node;
    struct _xmlAttr* attribute;
    
    uint64_t i;
    int buff_size;
    char size_buff[21]; //The max size of an uint64_t should be 20 characters
    
    struct _ds3_request* request;
    ds3_bulk_object_list* obj_list;
    ds3_bulk_object obj;
    ds3_xml_send_buff send_buff;

    GByteArray* xml_blob;
    GArray* objects_array; 

    xmlDocPtr doc;
    xmlChar* xml_buff;
    
    if(client == NULL || _request == NULL) {
        return _ds3_create_error(DS3_ERROR_MISSING_ARGS, "All arguments must be filled in for request processing"); 
    }
    
    request = (struct _ds3_request*) _request;

    if(request->object_list == NULL || request->object_list->size == 0) {
        return _ds3_create_error(DS3_ERROR_MISSING_ARGS, "The bulk command requires a list of objects to process"); 
    } 

    // Init the data structures declared above the null check
    memset(&send_buff, 0, sizeof(ds3_xml_send_buff));
    memset(&obj, 0, sizeof(ds3_bulk_object));
    obj_list = request->object_list;

    // Start creating the xml body to send to the server.
    doc = xmlNewDoc((xmlChar*)"1.0");
    root_node = xmlNewNode(NULL, (xmlChar*) "MasterObjectList");
    
    objects_node = xmlNewNode(NULL, (xmlChar*) "Objects");
    xmlAddChild(root_node, objects_node);
    
    for(i = 0; i < obj_list->size; i++) {
        obj = obj_list->list[i];
        memset(size_buff, 0, sizeof(char) * 21);
        g_snprintf(size_buff, sizeof(char) * 21, "%ld", obj.size);

        object_node = xmlNewNode(NULL, (xmlChar*) "Object");
        xmlAddChild(objects_node, object_node);

        xmlSetProp(object_node, (xmlChar*) "Name", (xmlChar*) obj.name);
        xmlSetProp(object_node, (xmlChar*) "Size", (xmlChar*) size_buff );
    }

    xmlDocSetRootElement(doc, root_node);
    xmlDocDumpFormatMemory(doc, &xml_buff, &buff_size, 1);

    send_buff.buff = (char*) xml_buff;
    send_buff.size = strlen(send_buff.buff);

    request->length = send_buff.size; // make sure to set the size of the request.

    xml_blob = g_byte_array_new();
    error_response = _net_process_request(client, request, xml_blob, load_xml_buff, (void*) &send_buff, _ds3_send_xml_buff);

    // Cleanup the data sent to the server.
    xmlFreeDoc(doc);
    xmlFree(xml_buff);
   
    if(error_response != NULL) {
        g_byte_array_free(xml_blob, TRUE);
        return error_response;
    }

    // Start processing the data that was received back.
    doc = xmlParseMemory((const char*) xml_blob->data, xml_blob->len);
    if(doc == NULL) {
        char* message = g_strconcat("Failed to parse response document.  The actual response is: ", xml_blob->data, NULL);
        g_byte_array_free(xml_blob, TRUE);
        ds3_error* error = _ds3_create_error(DS3_ERROR_INVALID_XML, message);
        g_free(message);
        return error;
    }

    root = xmlDocGetRootElement(doc);

    if(xmlStrcmp(root->name, (const xmlChar*) "MasterObjectList") != 0) {
        char* message = g_strconcat("Expected the root element to be 'MasterObjectList'.  The actual response is: ", xml_blob->data, NULL);
        xmlFreeDoc(doc);
        g_byte_array_free(xml_blob, TRUE);
        ds3_error* error = _ds3_create_error(DS3_ERROR_INVALID_XML, message);
        g_free(message);
        return error;
    }

    objects_array = g_array_new(FALSE, TRUE, sizeof(ds3_bulk_object_list*));

    response = g_new0(ds3_bulk_response, 1);

    for(attribute = root->properties; attribute != NULL; attribute = attribute->next) {
        if(xmlStrcmp(attribute->name, (const xmlChar*) "JobId") == 0) {
            text = xmlNodeListGetString(doc, attribute->children, 1);
            if(text == NULL) {
                continue;
            }
            response->job_id = g_strdup((char*) text);
            response->job_id_size = strlen((char*) text);
            xmlFree(text);
        }
        else {
            fprintf(stderr, "Unknown attribute: (%s)\n", attribute->name);
        }
    }

    for(child_node = root->xmlChildrenNode; child_node != NULL; child_node = child_node->next) {
        if(xmlStrcmp(child_node->name, (const xmlChar*) "Objects") == 0) {
            ds3_bulk_object_list* obj_list = _parse_bulk_objects(doc, child_node);
            g_array_append_val(objects_array, obj_list);
        }
        else {
            fprintf(stderr, "Unknown element: (%s)\n", child_node->name);
        }
    }

    response->list = (ds3_bulk_object_list**) objects_array->data;
    response->list_size = objects_array->len;
    
    xmlFreeDoc(doc);
    g_byte_array_free(xml_blob, TRUE);
    g_array_free(objects_array, FALSE);

    *_response = response;
    return NULL;
}


void ds3_print_request(const ds3_request* _request) {
    const struct _ds3_request* request; 
    if(_request == NULL) {
      fprintf(stderr, "Request object was null\n");
      return;
    }
    request = (struct _ds3_request*)_request;
    printf("Verb: %s\n", _net_get_verb(request->verb));
    printf("Path: %s\n", request->path);
}

void ds3_free_bucket_response(ds3_get_bucket_response* response){
    size_t num_objects;
    int i;
    if(response == NULL) {
        return;
    }

    num_objects = response->num_objects;

    for(i = 0; i < num_objects; i++) {
        ds3_object object = response->objects[i];
        g_free(object.name);
        g_free(object.etag);
        g_free(object.storage_class);
        g_free(object.last_modified);
        ds3_free_owner(object.owner);
    }

    g_free(response->objects);
    g_free(response->creation_date);
    g_free(response->marker);
    g_free(response->delimiter);
    g_free(response->name);
    g_free(response->next_marker);
    g_free(response->prefix);

    g_free(response);
}

void ds3_free_service_response(ds3_get_service_response* response){
    size_t num_buckets;
    int i;

    if(response == NULL) {
        return;
    }

    num_buckets = response->num_buckets;

    for(i = 0; i<num_buckets; i++) {
        ds3_bucket bucket = response->buckets[i];
        g_free(bucket.name);
        g_free(bucket.creation_date);
    }
    
    ds3_free_owner(response->owner);
    g_free(response->buckets);
    g_free(response);
}

void ds3_free_bulk_response(ds3_bulk_response* response) {
    int i;
    if(response == NULL) {
        fprintf(stderr, "Bulk response was NULL\n");
        return;
    }

    if(response->job_id != NULL) {
        g_free(response->job_id);
    }

    if (response->list != NULL ) {
        for (i = 0; i < response->list_size; i++) {
            ds3_free_bulk_object_list(response->list[i]);
        }
        g_free(response->list);
    }

    g_free(response);
}

void ds3_free_bucket(ds3_bucket* bucket) {
    if(bucket == NULL) {
        fprintf(stderr, "Bucket was NULL\n");
        return;
    }
    if(bucket->name != NULL) {
        g_free(bucket->name);
    }
    if(bucket->creation_date != NULL) {
        g_free(bucket->creation_date);
    }
    g_free(bucket);
}

void ds3_free_owner(ds3_owner* owner) {
    if(owner == NULL) {
        fprintf(stderr, "Owner was NULL\n");
        return;
    }
    if(owner->name != NULL) {
        g_free(owner->name);
    }
    if(owner->id != NULL) {
        g_free(owner->id);
    }
    g_free(owner);
}

void ds3_free_creds(ds3_creds* creds) {
    if(creds == NULL) {
        return;
    }

    if(creds->access_id != NULL) {
        g_free(creds->access_id);
    }

    if(creds->secret_key != NULL) {
        g_free(creds->secret_key);
    }
    g_free(creds);
}

void ds3_free_client(ds3_client* client) {
    if(client == NULL) {
      return;
    }
    if(client->endpoint != NULL) {
        g_free(client->endpoint);
    }
    if(client->proxy != NULL) {
        g_free(client->proxy);
    }
    g_free(client);
}

void ds3_free_request(ds3_request* _request) {
    struct _ds3_request* request; 
    if(_request == NULL) {
        return;
    }
    request = (struct _ds3_request*) _request;
    if(request->path != NULL) {
        g_free(request->path);
    }
    if(request->headers != NULL) {
        g_hash_table_destroy(request->headers);
    }
    if(request->query_params != NULL) {
        g_hash_table_destroy(request->query_params);
    }
    g_free(request);
}

void ds3_free_error(ds3_error* error) {
    if(error == NULL) {
        return;
    }

    if(error->message != NULL) {
        g_free(error->message);
    }

    g_free(error);
}

void ds3_cleanup(void) {
    net_cleanup();
}

size_t ds3_write_to_file(void* buffer, size_t size, size_t nmemb, void* user_data) {
    return fwrite(buffer, size, nmemb, (FILE*) user_data);
}

size_t ds3_read_from_file(void* buffer, size_t size, size_t nmemb, void* user_data) {
    return fread(buffer, size, nmemb, (FILE*) user_data);
}

static ds3_bulk_object _ds3_bulk_object_from_file(const char* file_name) {
    struct stat file_info;
    int result; 
    memset(&file_info, 0, sizeof(struct stat));

    result = stat(file_name, &file_info);
    if (result != 0) {
        fprintf(stderr, "Failed to get file info for %s\n", file_name);
    }

    ds3_bulk_object obj;
    memset(&obj, 0, sizeof(ds3_bulk_object));

    obj.name = g_strdup(file_name);
    obj.name_size = strlen(file_name);
    obj.size = file_info.st_size;

    return obj;
}

ds3_bulk_object_list* ds3_convert_file_list(const char** file_list, uint64_t num_files) {
    uint64_t i;
    ds3_bulk_object_list* obj_list = g_new0(ds3_bulk_object_list, 1);
    obj_list->size = num_files;
    obj_list->list = g_new0(ds3_bulk_object, num_files);
    
    for(i = 0; i < num_files; i++) {
        obj_list->list[i] = _ds3_bulk_object_from_file(file_list[i]);
    }

    return obj_list;
}

void ds3_free_bulk_object_list(ds3_bulk_object_list* object_list) {
    uint64_t i, count;
    if(object_list == NULL) {
        return;
    }
    count = object_list->size;
    for(i = 0; i < count; i++) {
        char* file_name = object_list->list[i].name;
        if (file_name == NULL) {
            continue;
        }
        g_free(file_name);
    }

    if(object_list->server_id != NULL) {
        g_free(object_list->server_id);
    }

    g_free(object_list->list);
    g_free(object_list);
}

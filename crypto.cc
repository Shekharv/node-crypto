#include <node.h>
#include <node_events.h>
#include <assert.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/hmac.h>
#include <openssl/err.h>

#define EVP_F_EVP_DECRYPTFINAL 101

using namespace v8;
using namespace node;

void hex_encode(unsigned char *md_value, int md_len, char** md_hexdigest, int* md_hex_len) {
  *md_hex_len = (2*(md_len));
  *md_hexdigest = (char *) malloc(*md_hex_len + 1);
  for(int i = 0; i < md_len; i++) {
    sprintf((char *)(*md_hexdigest + (i*2)), "%02x",  md_value[i]);
  }
}

#define hex2i(c) ((c) <= '9' ? ((c) - '0') : (c) <= 'Z' ? ((c) - 'A' + 10) : ((c) - 'a' + 10))
void hex_decode(unsigned char *input, int length, char** buf64, int* buf64_len) {
  *buf64_len = (length/2);
  *buf64 = (char*) malloc(length/2 + 1);
  char *b = *buf64;
  for(int i = 0; i < length-1; i+=2) {
    b[i/2]  = (hex2i(input[i])<<4) | (hex2i(input[i+1]));
  }
}

void base64(unsigned char *input, int length, char** buf64, int* buf64_len)
{
  BIO *bmem, *b64;
  BUF_MEM *bptr;

  b64 = BIO_new(BIO_f_base64());
  bmem = BIO_new(BIO_s_mem());
  b64 = BIO_push(b64, bmem);
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO_write(b64, input, length);
  BIO_flush(b64);
  BIO_get_mem_ptr(b64, &bptr);

  *buf64_len = bptr->length;
  *buf64 = (char *)malloc(*buf64_len+1);
  memcpy(*buf64, bptr->data, bptr->length);
  char* b = *buf64;
  b[bptr->length] = 0;

  BIO_free_all(b64);

}

void *unbase64(unsigned char *input, int length, char** buffer, int* buffer_len)
{
  BIO *b64, *bmem;
  *buffer = (char *)malloc(length);
  memset(*buffer, 0, length);

  b64 = BIO_new(BIO_f_base64());
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  bmem = BIO_new_mem_buf(input, length);
  bmem = BIO_push(b64, bmem);

  *buffer_len = BIO_read(bmem, *buffer, length);
  BIO_free_all(bmem);

}


// LengthWithoutIncompleteUtf8 from V8 d8-posix.cc
// see http://v8.googlecode.com/svn/trunk/src/d8-posix.cc
static int LengthWithoutIncompleteUtf8(char* buffer, int len) {
  int answer = len;
  // 1-byte encoding.
  static const int kUtf8SingleByteMask = 0x80;
  static const int kUtf8SingleByteValue = 0x00;
  // 2-byte encoding.
  static const int kUtf8TwoByteMask = 0xe0;
  static const int kUtf8TwoByteValue = 0xc0;
  // 3-byte encoding.
  static const int kUtf8ThreeByteMask = 0xf0;
  static const int kUtf8ThreeByteValue = 0xe0;
  // 4-byte encoding.
  static const int kUtf8FourByteMask = 0xf8;
  static const int kUtf8FourByteValue = 0xf0;
  // Subsequent bytes of a multi-byte encoding.
  static const int kMultiByteMask = 0xc0;
  static const int kMultiByteValue = 0x80;
  int multi_byte_bytes_seen = 0;
  while (answer > 0) {
    int c = buffer[answer - 1];
    // Ends in valid single-byte sequence?
    if ((c & kUtf8SingleByteMask) == kUtf8SingleByteValue) return answer;
    // Ends in one or more subsequent bytes of a multi-byte value?
    if ((c & kMultiByteMask) == kMultiByteValue) {
      multi_byte_bytes_seen++;
      answer--;
    } else {
      if ((c & kUtf8TwoByteMask) == kUtf8TwoByteValue) {
        if (multi_byte_bytes_seen >= 1) {
          return answer + 2;
        }
        return answer - 1;
      } else if ((c & kUtf8ThreeByteMask) == kUtf8ThreeByteValue) {
        if (multi_byte_bytes_seen >= 2) {
          return answer + 3;
        }
        return answer - 1;
      } else if ((c & kUtf8FourByteMask) == kUtf8FourByteValue) {
        if (multi_byte_bytes_seen >= 3) {
          return answer + 4;
        }
        return answer - 1;
      } else {
        return answer;  // Malformed UTF-8.
      }
    }
  }
  return 0;
}

// local decrypt final without strict padding check
// to work with php mcrypt
// see http://www.mail-archive.com/openssl-dev@openssl.org/msg19927.html
int local_EVP_DecryptFinal_ex(EVP_CIPHER_CTX *ctx, unsigned char *out, int *outl)
{
  int i,b;
  int n;

  *outl=0;
  b=ctx->cipher->block_size;
  if (ctx->flags & EVP_CIPH_NO_PADDING)
    {
      if(ctx->buf_len)
	{
	  EVPerr(EVP_F_EVP_DECRYPTFINAL,EVP_R_DATA_NOT_MULTIPLE_OF_BLOCK_LENGTH);
	  return 0;
	}
      *outl = 0;
      return 1;
    }
  if (b > 1)
    {
      if (ctx->buf_len || !ctx->final_used)
	{
	  EVPerr(EVP_F_EVP_DECRYPTFINAL,EVP_R_WRONG_FINAL_BLOCK_LENGTH);
	  return(0);
	}
      OPENSSL_assert(b <= sizeof ctx->final);
      n=ctx->final[b-1];
      if (n > b)
	{
	  EVPerr(EVP_F_EVP_DECRYPTFINAL,EVP_R_BAD_DECRYPT);
	  return(0);
	}
      for (i=0; i<n; i++)
	{
	  if (ctx->final[--b] != n)
	    {
	      EVPerr(EVP_F_EVP_DECRYPTFINAL,EVP_R_BAD_DECRYPT);
	      return(0);
	    }
	}
      n=ctx->cipher->block_size-n;
      for (i=0; i<n; i++)
	out[i]=ctx->final[i];
      *outl=n;
    }
  else
    *outl=0;
  return(1);
}



class Cipher : public ObjectWrap {
 public:
  static void
  Initialize (v8::Handle<v8::Object> target)
  {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    t->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(t, "init", CipherInit);
    NODE_SET_PROTOTYPE_METHOD(t, "initiv", CipherInitIv);
    NODE_SET_PROTOTYPE_METHOD(t, "update", CipherUpdate);
    NODE_SET_PROTOTYPE_METHOD(t, "final", CipherFinal);

    target->Set(String::NewSymbol("Cipher"), t->GetFunction());
  }

  bool CipherInit(char* cipherType, char* key_buf, int key_buf_len)
  {
    cipher = EVP_get_cipherbyname(cipherType);
    if(!cipher) {
      fprintf(stderr, "node-crypto : Unknown cipher %s\n", cipherType);
      return false;
    }

    unsigned char key[EVP_MAX_KEY_LENGTH],iv[EVP_MAX_IV_LENGTH];
    int key_len = EVP_BytesToKey(cipher, EVP_md5(), NULL, (unsigned char*) key_buf, key_buf_len, 1, key, iv);

    EVP_CIPHER_CTX_init(&ctx);
    EVP_CipherInit(&ctx,cipher,(unsigned char *)key,(unsigned char *)iv, true);
    if (!EVP_CIPHER_CTX_set_key_length(&ctx,key_len)) {
    	fprintf(stderr, "node-crypto : Invalid key length %d\n", key_len);
    	EVP_CIPHER_CTX_cleanup(&ctx);
    	return false;
    }
    initialised = true;
    return true;
  }


  bool CipherInitIv(char* cipherType, char* key, int key_len, char *iv, int iv_len)
  {
    cipher = EVP_get_cipherbyname(cipherType);
    if(!cipher) {
      fprintf(stderr, "node-crypto : Unknown cipher %s\n", cipherType);
      return false;
    }
    if (EVP_CIPHER_iv_length(cipher)!=iv_len) {
    	fprintf(stderr, "node-crypto : Invalid IV length %d\n", iv_len);
      return false;
    }
    EVP_CIPHER_CTX_init(&ctx);
    EVP_CipherInit(&ctx,cipher,(unsigned char *)key,(unsigned char *)iv, true);
    if (!EVP_CIPHER_CTX_set_key_length(&ctx,key_len)) {
    	fprintf(stderr, "node-crypto : Invalid key length %d\n", key_len);
    	EVP_CIPHER_CTX_cleanup(&ctx);
    	return false;
    }
    initialised = true;
    return true;
  }


  int CipherUpdate(char* data, int len, unsigned char** out, int* out_len) {
    if (!initialised)
      return 0;
    *out_len=len+EVP_CIPHER_CTX_block_size(&ctx);
    *out=(unsigned char*)malloc(*out_len);
    
    EVP_CipherUpdate(&ctx, *out, out_len, (unsigned char*)data, len);
    return 1;
  }

  int CipherFinal(unsigned char** out, int *out_len) {
    if (!initialised)
      return 0;
    *out = (unsigned char*) malloc(EVP_CIPHER_CTX_block_size(&ctx));
    EVP_CipherFinal(&ctx,*out,out_len);
    EVP_CIPHER_CTX_cleanup(&ctx);
    initialised = false;
    return 1;
  }


 protected:

  static Handle<Value>
  New (const Arguments& args)
  {
    HandleScope scope;

    Cipher *cipher = new Cipher();
    cipher->Wrap(args.This());
    return args.This();
  }

  static Handle<Value>
  CipherInit(const Arguments& args) {
    Cipher *cipher = ObjectWrap::Unwrap<Cipher>(args.This());
		
    HandleScope scope;

    cipher->incomplete_base64=NULL;

    if (args.Length() <= 1 || !args[0]->IsString() || !args[1]->IsString()) {
      return ThrowException(String::New("Must give cipher-type, key"));
    }
    

    ssize_t key_buf_len = DecodeBytes(args[1], BINARY);

    if (key_buf_len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }
    
    char* key_buf = new char[key_buf_len];
    ssize_t key_written = DecodeWrite(key_buf, key_buf_len, args[1], BINARY);
    assert(key_written == key_buf_len);
    
    String::Utf8Value cipherType(args[0]->ToString());

    bool r = cipher->CipherInit(*cipherType, key_buf, key_buf_len);

    return args.This();
  }

  static Handle<Value>
  CipherInitIv(const Arguments& args) {
    Cipher *cipher = ObjectWrap::Unwrap<Cipher>(args.This());
		
    HandleScope scope;

    cipher->incomplete_base64=NULL;

    if (args.Length() <= 2 || !args[0]->IsString() || !args[1]->IsString() || !args[2]->IsString()) {
      return ThrowException(String::New("Must give cipher-type, key, and iv as argument"));
    }
    ssize_t key_len = DecodeBytes(args[1], BINARY);

    if (key_len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }
    
    ssize_t iv_len = DecodeBytes(args[2], BINARY);

    if (iv_len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* key_buf = new char[key_len];
    ssize_t key_written = DecodeWrite(key_buf, key_len, args[1], BINARY);
    assert(key_written == key_len);
    
    char* iv_buf = new char[iv_len];
    ssize_t iv_written = DecodeWrite(iv_buf, iv_len, args[2], BINARY);
    assert(iv_written == iv_len);

    String::Utf8Value cipherType(args[0]->ToString());
    	
    bool r = cipher->CipherInitIv(*cipherType, key_buf,key_len,iv_buf,iv_len);

    return args.This();
  }


  static Handle<Value>
  CipherUpdate(const Arguments& args) {
    Cipher *cipher = ObjectWrap::Unwrap<Cipher>(args.This());

    HandleScope scope;

    enum encoding enc = ParseEncoding(args[1]);
    ssize_t len = DecodeBytes(args[0], enc);

    if (len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* buf = new char[len];
    ssize_t written = DecodeWrite(buf, len, args[0], enc);
    assert(written == len);

    unsigned char *out=0;
    int out_len=0;
    int r = cipher->CipherUpdate(buf, len,&out,&out_len);
    
    Local<Value> outString;
    if (out_len==0) outString=String::New("");
    else {
    	if (args.Length() <= 2 || !args[2]->IsString()) {
	      // Binary
	      outString = Encode(out, out_len, BINARY);
	    } else {
	      char* out_hexdigest;
	      int out_hex_len;
	      String::Utf8Value encoding(args[2]->ToString());
	      if (strcasecmp(*encoding, "hex") == 0) {
	        // Hex encoding
	        hex_encode(out, out_len, &out_hexdigest, &out_hex_len);
	        outString = Encode(out_hexdigest, out_hex_len, BINARY);
	        free(out_hexdigest);
	      } else if (strcasecmp(*encoding, "base64") == 0) {
		// Base64 encoding
		// Check to see if we need to add in previous base64 overhang
		if (cipher->incomplete_base64!=NULL){
		  unsigned char* complete_base64 = (unsigned char *)malloc(out_len+cipher->incomplete_base64_len+1);
		  memcpy(complete_base64, cipher->incomplete_base64, cipher->incomplete_base64_len);
		  memcpy(&complete_base64[cipher->incomplete_base64_len], out, out_len);
		  free(out);
		  free(cipher->incomplete_base64);
		  cipher->incomplete_base64=NULL;
		  out=complete_base64;
		  out_len += cipher->incomplete_base64_len;
		}

		// Check to see if we need to trim base64 stream
		if (out_len%3!=0){
		  cipher->incomplete_base64_len = out_len%3;
		  cipher->incomplete_base64 = (char *)malloc(cipher->incomplete_base64_len+1);
		  memcpy(cipher->incomplete_base64, &out[out_len-cipher->incomplete_base64_len], cipher->incomplete_base64_len);
		  out_len -= cipher->incomplete_base64_len;
		  out[out_len]=0;
		}

	        base64(out, out_len, &out_hexdigest, &out_hex_len);
	        outString = Encode(out_hexdigest, out_hex_len, BINARY);
	        free(out_hexdigest);
	      } else if (strcasecmp(*encoding, "binary") == 0) {
	        outString = Encode(out, out_len, BINARY);
	      } else {
		fprintf(stderr, "node-crypto : Cipher .update encoding "
			"can be binary, hex or base64\n");
	      }
	    }
    }
    if (out) free(out);
    return scope.Close(outString);
  }

  static Handle<Value>
  CipherFinal(const Arguments& args) {
    Cipher *cipher = ObjectWrap::Unwrap<Cipher>(args.This());

    HandleScope scope;

    unsigned char* out_value;
    int out_len;
    char* out_hexdigest;
    int out_hex_len;
    Local<Value> outString ;

    int r = cipher->CipherFinal(&out_value, &out_len);

    if (out_len == 0 || r == 0) {
      return scope.Close(String::New(""));
    }

    if (args.Length() == 0 || !args[0]->IsString()) {
      // Binary
      outString = Encode(out_value, out_len, BINARY);
    } else {
      String::Utf8Value encoding(args[0]->ToString());
      if (strcasecmp(*encoding, "hex") == 0) {
        // Hex encoding
        hex_encode(out_value, out_len, &out_hexdigest, &out_hex_len);
        outString = Encode(out_hexdigest, out_hex_len, BINARY);
        free(out_hexdigest);
      } else if (strcasecmp(*encoding, "base64") == 0) {
        base64(out_value, out_len, &out_hexdigest, &out_hex_len);
        outString = Encode(out_hexdigest, out_hex_len, BINARY);
        free(out_hexdigest);
      } else if (strcasecmp(*encoding, "binary") == 0) {
        outString = Encode(out_value, out_len, BINARY);
      } else {
	fprintf(stderr, "node-crypto : Cipher .final encoding "
		"can be binary, hex or base64\n");
      }
    }
    free(out_value);
    return scope.Close(outString);

  }

  Cipher () : ObjectWrap () 
  {
    initialised = false;
  }

  ~Cipher ()
  {
  }

 private:

  EVP_CIPHER_CTX ctx;
  const EVP_CIPHER *cipher;
  bool initialised;
  char* incomplete_base64;
  int incomplete_base64_len;

};



class Decipher : public ObjectWrap {
 public:
  static void
  Initialize (v8::Handle<v8::Object> target)
  {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    t->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(t, "init", DecipherInit);
    NODE_SET_PROTOTYPE_METHOD(t, "initiv", DecipherInitIv);
    NODE_SET_PROTOTYPE_METHOD(t, "update", DecipherUpdate);
    NODE_SET_PROTOTYPE_METHOD(t, "final", DecipherFinal);
    NODE_SET_PROTOTYPE_METHOD(t, "finaltol", DecipherFinalTolerate);

    target->Set(String::NewSymbol("Decipher"), t->GetFunction());
  }

  bool DecipherInit(char* cipherType, char* key_buf, int key_buf_len)
  {
    cipher = EVP_get_cipherbyname(cipherType);
    if(!cipher) {
      fprintf(stderr, "node-crypto : Unknown cipher %s\n", cipherType);
      return false;
    }

    unsigned char key[EVP_MAX_KEY_LENGTH],iv[EVP_MAX_IV_LENGTH];
    int key_len = EVP_BytesToKey(cipher, EVP_md5(), NULL, (unsigned char*) key_buf, key_buf_len, 1, key, iv);

    EVP_CIPHER_CTX_init(&ctx);
    EVP_CipherInit(&ctx,cipher,(unsigned char *)key,(unsigned char *)iv, false);
    if (!EVP_CIPHER_CTX_set_key_length(&ctx,key_len)) {
    	fprintf(stderr, "node-crypto : Invalid key length %d\n", key_len);
    	EVP_CIPHER_CTX_cleanup(&ctx);
    	return false;
    }
    initialised = true;
    return true;
  }


  bool DecipherInitIv(char* cipherType, char* key, int key_len, char *iv, int iv_len)
  {
    cipher = EVP_get_cipherbyname(cipherType);
    if(!cipher) {
      fprintf(stderr, "node-crypto : Unknown cipher %s\n", cipherType);
      return false;
    }
    if (EVP_CIPHER_iv_length(cipher)!=iv_len) {
    	fprintf(stderr, "node-crypto : Invalid IV length %d\n", iv_len);
      return false;
    }
    EVP_CIPHER_CTX_init(&ctx);
    EVP_CipherInit(&ctx,cipher,(unsigned char *)key,(unsigned char *)iv, false);
    if (!EVP_CIPHER_CTX_set_key_length(&ctx,key_len)) {
    	fprintf(stderr, "node-crypto : Invalid key length %d\n", key_len);
    	EVP_CIPHER_CTX_cleanup(&ctx);
    	return false;
    }
    initialised = true;
    return true;
  }

  int DecipherUpdate(char* data, int len, unsigned char** out, int* out_len) {
    if (!initialised)
      return 0;
    *out_len=len+EVP_CIPHER_CTX_block_size(&ctx);
    *out=(unsigned char*)malloc(*out_len);
    
    EVP_CipherUpdate(&ctx, *out, out_len, (unsigned char*)data, len);
    return 1;
  }

  int DecipherFinal(unsigned char** out, int *out_len, bool tolerate_padding) {
    if (!initialised)
      return 0;
    *out = (unsigned char*) malloc(EVP_CIPHER_CTX_block_size(&ctx));
    if (tolerate_padding) {
      local_EVP_DecryptFinal_ex(&ctx,*out,out_len);
    } else {
      EVP_CipherFinal(&ctx,*out,out_len);
    }
    EVP_CIPHER_CTX_cleanup(&ctx);
    initialised = false;
    return 1;
  }


 protected:

  static Handle<Value>
  New (const Arguments& args)
  {
    HandleScope scope;

    Decipher *cipher = new Decipher();
    cipher->Wrap(args.This());
    return args.This();
  }

  static Handle<Value>
  DecipherInit(const Arguments& args) {
    Decipher *cipher = ObjectWrap::Unwrap<Decipher>(args.This());
		
    HandleScope scope;

    cipher->incomplete_utf8=NULL;
    cipher->incomplete_hex_flag=false;

    if (args.Length() <= 1 || !args[0]->IsString() || !args[1]->IsString()) {
      return ThrowException(String::New("Must give cipher-type, key as argument"));
    }

    ssize_t key_len = DecodeBytes(args[1], BINARY);

    if (key_len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }
    
    char* key_buf = new char[key_len];
    ssize_t key_written = DecodeWrite(key_buf, key_len, args[1], BINARY);
    assert(key_written == key_len);
    
    String::Utf8Value cipherType(args[0]->ToString());
    	
    bool r = cipher->DecipherInit(*cipherType, key_buf,key_len);

    return args.This();
  }

  static Handle<Value>
  DecipherInitIv(const Arguments& args) {
    Decipher *cipher = ObjectWrap::Unwrap<Decipher>(args.This());
		
    HandleScope scope;

    cipher->incomplete_utf8=NULL;
    cipher->incomplete_hex_flag=false;

    if (args.Length() <= 2 || !args[0]->IsString() || !args[1]->IsString() || !args[2]->IsString()) {
      return ThrowException(String::New("Must give cipher-type, key, and iv as argument"));
    }

    ssize_t key_len = DecodeBytes(args[1], BINARY);

    if (key_len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }
    
    ssize_t iv_len = DecodeBytes(args[2], BINARY);

    if (iv_len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* key_buf = new char[key_len];
    ssize_t key_written = DecodeWrite(key_buf, key_len, args[1], BINARY);
    assert(key_written == key_len);
    
    char* iv_buf = new char[iv_len];
    ssize_t iv_written = DecodeWrite(iv_buf, iv_len, args[2], BINARY);
    assert(iv_written == iv_len);

    String::Utf8Value cipherType(args[0]->ToString());
    	
    bool r = cipher->DecipherInitIv(*cipherType, key_buf,key_len,iv_buf,iv_len);

    return args.This();
  }

  static Handle<Value>
  DecipherUpdate(const Arguments& args) {
    Decipher *cipher = ObjectWrap::Unwrap<Decipher>(args.This());

    HandleScope scope;

    ssize_t len = DecodeBytes(args[0], BINARY);
    char* buf = new char[len];
    ssize_t written = DecodeWrite(buf, len, args[0], BINARY);
    char* ciphertext;
    int ciphertext_len;


    if (args.Length() <= 1 || !args[1]->IsString()) {
      // Binary - do nothing
    } else {
      String::Utf8Value encoding(args[1]->ToString());
      if (strcasecmp(*encoding, "hex") == 0) {
	// Hex encoding
	// Do we have a previous hex carry over?
	if (cipher->incomplete_hex_flag) {
	  char* complete_hex = (char*)malloc(len+2);
	  memcpy(complete_hex, &cipher->incomplete_hex, 1);
	  memcpy(complete_hex+1, buf, len);
	  free(buf);
	  buf = complete_hex;
	  len += 1;
	}
	// Do we have an incomplete hex stream?
	if ((len>0) && (len % 2 !=0)) {
	  len--;
	  cipher->incomplete_hex=buf[len];
	  cipher->incomplete_hex_flag=true;
	  buf[len]=0;
	}
        hex_decode((unsigned char*)buf, len, (char **)&ciphertext, &ciphertext_len);

        free(buf);
	buf = ciphertext;
	len = ciphertext_len;
      } else if (strcasecmp(*encoding, "base64") == 0) {
        unbase64((unsigned char*)buf, len, (char **)&ciphertext, &ciphertext_len);
        free(buf);
	buf = ciphertext;
	len = ciphertext_len;
      } else if (strcasecmp(*encoding, "binary") == 0) {
        // Binary - do nothing
      } else {
	fprintf(stderr, "node-crypto : Decipher .update encoding "
		"can be binary, hex or base64\n");
      }
  
    }

    unsigned char *out=0;
    int out_len=0;
    int r = cipher->DecipherUpdate(buf, len,&out,&out_len);

    Local<Value> outString;
    if (out_len==0) {
      outString=String::New("");
    } else if (args.Length() <= 2 || !args[2]->IsString()) {
      outString = Encode(out, out_len, BINARY);
    } else {
      enum encoding enc = ParseEncoding(args[2]);
      if (enc == UTF8) {
	// See if we have any overhang from last utf8 partial ending
	if (cipher->incomplete_utf8!=NULL) {
	  char* complete_out = (char *)malloc(cipher->incomplete_utf8_len + out_len);
	  memcpy(complete_out, cipher->incomplete_utf8, cipher->incomplete_utf8_len);
	  memcpy((char *)complete_out+cipher->incomplete_utf8_len, out, out_len);
	  free(out);
	  free(cipher->incomplete_utf8);
	  cipher->incomplete_utf8=NULL;
	  out = (unsigned char*)complete_out;
	  out_len += cipher->incomplete_utf8_len;
	}
	// Check to see if we have a complete utf8 stream
	int utf8_len = LengthWithoutIncompleteUtf8((char *)out, out_len);
	if (utf8_len<out_len) { // We have an incomplete ut8 ending
	  cipher->incomplete_utf8_len = out_len-utf8_len;
          cipher->incomplete_utf8 = (unsigned char *)malloc(cipher->incomplete_utf8_len+1);
          memcpy(cipher->incomplete_utf8, &out[utf8_len], cipher->incomplete_utf8_len);
	} 
        outString = Encode(out, utf8_len, enc);
      } else {
        outString = Encode(out, out_len, enc);
      }
    }

    if (out) free(out);
    free(buf);
    return scope.Close(outString);

  }

  static Handle<Value>
  DecipherFinal(const Arguments& args) {
    Decipher *cipher = ObjectWrap::Unwrap<Decipher>(args.This());

    HandleScope scope;

    unsigned char* out_value;
    int out_len;
    char* out_hexdigest;
    int out_hex_len;
    Local<Value> outString ;

    int r = cipher->DecipherFinal(&out_value, &out_len, false);

    if (out_len == 0 || r == 0) {
      return scope.Close(String::New(""));
    }


    if (args.Length() == 0 || !args[0]->IsString()) {
      outString = Encode(out_value, out_len, BINARY);
    } else {
      enum encoding enc = ParseEncoding(args[0]);
      if (enc == UTF8) {
	// See if we have any overhang from last utf8 partial ending
	if (cipher->incomplete_utf8!=NULL) {
	  char* complete_out = (char *)malloc(cipher->incomplete_utf8_len + out_len);
	  memcpy(complete_out, cipher->incomplete_utf8, cipher->incomplete_utf8_len);
	  memcpy((char *)complete_out+cipher->incomplete_utf8_len, out_value, out_len);
	  free(cipher->incomplete_utf8);
	  cipher->incomplete_utf8=NULL;
	  outString = Encode(complete_out, cipher->incomplete_utf8_len+out_len, enc);
	  free(complete_out);
	} else {
	  outString = Encode(out_value, out_len, enc);
	}
      } else {
	outString = Encode(out_value, out_len, enc);
      }
    }
    free(out_value);
    return scope.Close(outString);

  }

  static Handle<Value>
  DecipherFinalTolerate(const Arguments& args) {
    Decipher *cipher = ObjectWrap::Unwrap<Decipher>(args.This());

    HandleScope scope;

    unsigned char* out_value;
    int out_len;
    char* out_hexdigest;
    int out_hex_len;
    Local<Value> outString ;

    int r = cipher->DecipherFinal(&out_value, &out_len, true);

    if (out_len == 0 || r == 0) {
      return scope.Close(String::New(""));
    }


    if (args.Length() == 0 || !args[0]->IsString()) {
      outString = Encode(out_value, out_len, BINARY);
    } else {
      enum encoding enc = ParseEncoding(args[0]);
      if (enc == UTF8) {
	// See if we have any overhang from last utf8 partial ending
	if (cipher->incomplete_utf8!=NULL) {
	  char* complete_out = (char *)malloc(cipher->incomplete_utf8_len + out_len);
	  memcpy(complete_out, cipher->incomplete_utf8, cipher->incomplete_utf8_len);
	  memcpy((char *)complete_out+cipher->incomplete_utf8_len, out_value, out_len);
	  free(cipher->incomplete_utf8);
	  cipher->incomplete_utf8=NULL;
	  outString = Encode(complete_out, cipher->incomplete_utf8_len+out_len, enc);
	  free(complete_out);
	} else {
	  outString = Encode(out_value, out_len, enc);
	}
      } else {
	outString = Encode(out_value, out_len, enc);
      }
    }
    free(out_value);
    return scope.Close(outString);

  }

  Decipher () : ObjectWrap () 
  {
    initialised = false;
  }

  ~Decipher ()
  {
  }

 private:

  EVP_CIPHER_CTX ctx;
  const EVP_CIPHER *cipher;
  bool initialised;
  unsigned char* incomplete_utf8;
  int incomplete_utf8_len;
  char incomplete_hex;
  bool incomplete_hex_flag;
};




class Hmac : public ObjectWrap {
 public:
  static void
  Initialize (v8::Handle<v8::Object> target)
  {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    t->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(t, "init", HmacInit);
    NODE_SET_PROTOTYPE_METHOD(t, "update", HmacUpdate);
    NODE_SET_PROTOTYPE_METHOD(t, "digest", HmacDigest);

    target->Set(String::NewSymbol("Hmac"), t->GetFunction());
  }

  bool HmacInit(char* hashType, char* key, int key_len)
  {
    md = EVP_get_digestbyname(hashType);
    if(!md) {
      fprintf(stderr, "node-crypto : Unknown message digest %s\n", hashType);
      return false;
    }
    HMAC_CTX_init(&ctx);
    HMAC_Init(&ctx, key, key_len, md);
    initialised = true;
    return true;
    
  }

  int HmacUpdate(char* data, int len) {
    if (!initialised)
      return 0;
    HMAC_Update(&ctx, (unsigned char*)data, len);
    return 1;
  }

  int HmacDigest(unsigned char** md_value, unsigned int *md_len) {
    if (!initialised)
      return 0;
    *md_value = (unsigned char*) malloc(EVP_MAX_MD_SIZE);
    HMAC_Final(&ctx, *md_value, md_len);
    HMAC_CTX_cleanup(&ctx);
    initialised = false;
    return 1;
  }


 protected:

  static Handle<Value>
  New (const Arguments& args)
  {
    HandleScope scope;

    Hmac *hmac = new Hmac();
    hmac->Wrap(args.This());
    return args.This();
  }

  static Handle<Value>
  HmacInit(const Arguments& args) {
    Hmac *hmac = ObjectWrap::Unwrap<Hmac>(args.This());

    HandleScope scope;

    if (args.Length() == 0 || !args[0]->IsString()) {
      return ThrowException(String::New("Must give hashtype string as argument"));
    }

    ssize_t len = DecodeBytes(args[1], BINARY);

    if (len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* buf = new char[len];
    ssize_t written = DecodeWrite(buf, len, args[1], BINARY);
    assert(written == len);

    String::Utf8Value hashType(args[0]->ToString());

    bool r = hmac->HmacInit(*hashType, buf, len);

    return args.This();
  }

  static Handle<Value>
  HmacUpdate(const Arguments& args) {
    Hmac *hmac = ObjectWrap::Unwrap<Hmac>(args.This());

    HandleScope scope;

    enum encoding enc = ParseEncoding(args[1]);
    ssize_t len = DecodeBytes(args[0], enc);

    if (len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* buf = new char[len];
    ssize_t written = DecodeWrite(buf, len, args[0], enc);
    assert(written == len);

    int r = hmac->HmacUpdate(buf, len);

    return args.This();
  }

  static Handle<Value>
  HmacDigest(const Arguments& args) {
    Hmac *hmac = ObjectWrap::Unwrap<Hmac>(args.This());

    HandleScope scope;

    unsigned char* md_value;
    unsigned int md_len;
    char* md_hexdigest;
    int md_hex_len;
    Local<Value> outString ;

    int r = hmac->HmacDigest(&md_value, &md_len);

    if (md_len == 0 || r == 0) {
      return scope.Close(String::New(""));
    }

    if (args.Length() == 0 || !args[0]->IsString()) {
      // Binary
      outString = Encode(md_value, md_len, BINARY);
    } else {
      String::Utf8Value encoding(args[0]->ToString());
      if (strcasecmp(*encoding, "hex") == 0) {
        // Hex encoding
        hex_encode(md_value, md_len, &md_hexdigest, &md_hex_len);
        outString = Encode(md_hexdigest, md_hex_len, BINARY);
        free(md_hexdigest);
      } else if (strcasecmp(*encoding, "base64") == 0) {
        base64(md_value, md_len, &md_hexdigest, &md_hex_len);
        outString = Encode(md_hexdigest, md_hex_len, BINARY);
        free(md_hexdigest);
      } else if (strcasecmp(*encoding, "binary") == 0) {
        outString = Encode(md_value, md_len, BINARY);
      } else {
	fprintf(stderr, "node-crypto : Hmac .digest encoding "
		"can be binary, hex or base64\n");
      }
    }
    free(md_value);
    return scope.Close(outString);

  }

  Hmac () : ObjectWrap () 
  {
    initialised = false;
  }

  ~Hmac ()
  {
  }

 private:

  HMAC_CTX ctx;
  const EVP_MD *md;
  bool initialised;

};


class Hash : public ObjectWrap {
 public:
  static void
  Initialize (v8::Handle<v8::Object> target)
  {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    t->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(t, "init", HashInit);
    NODE_SET_PROTOTYPE_METHOD(t, "update", HashUpdate);
    NODE_SET_PROTOTYPE_METHOD(t, "digest", HashDigest);

    target->Set(String::NewSymbol("Hash"), t->GetFunction());
  }

  bool HashInit (const char* hashType)
  {
    md = EVP_get_digestbyname(hashType);
    if(!md) {
      fprintf(stderr, "node-crypto : Unknown message digest %s\n", hashType);
      return false;
    }
    EVP_MD_CTX_init(&mdctx);
    EVP_DigestInit_ex(&mdctx, md, NULL);
    initialised = true;
    return true;
    
  }

  int HashUpdate(char* data, int len) {
    if (!initialised)
      return 0;
    EVP_DigestUpdate(&mdctx, data, len);
    return 1;
  }

  int HashDigest(unsigned char** md_value, unsigned int *md_len) {
    if (!initialised)
      return 0;
    *md_value = (unsigned char*) malloc(EVP_MAX_MD_SIZE);
    EVP_DigestFinal_ex(&mdctx, *md_value, md_len);
    EVP_MD_CTX_cleanup(&mdctx);
    initialised = false;
    return 1;
  }


 protected:

  static Handle<Value>
  New (const Arguments& args)
  {
    HandleScope scope;

    Hash *hash = new Hash();
    hash->Wrap(args.This());
    return args.This();
  }

  static Handle<Value>
  HashInit(const Arguments& args) {
    Hash *hash = ObjectWrap::Unwrap<Hash>(args.This());

    HandleScope scope;

    if (args.Length() == 0 || !args[0]->IsString()) {
      return ThrowException(String::New("Must give hashtype string as argument"));
    }

    String::Utf8Value hashType(args[0]->ToString());

    bool r = hash->HashInit(*hashType);

    return args.This();
  }

  static Handle<Value>
  HashUpdate(const Arguments& args) {
    Hash *hash = ObjectWrap::Unwrap<Hash>(args.This());

    HandleScope scope;

    enum encoding enc = ParseEncoding(args[1]);
    ssize_t len = DecodeBytes(args[0], enc);

    if (len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* buf = new char[len];
    ssize_t written = DecodeWrite(buf, len, args[0], enc);
    assert(written == len);

    int r = hash->HashUpdate(buf, len);

    return args.This();
  }

  static Handle<Value>
  HashDigest(const Arguments& args) {
    Hash *hash = ObjectWrap::Unwrap<Hash>(args.This());

    HandleScope scope;

    unsigned char* md_value;
    unsigned int md_len;
    char* md_hexdigest;
    int md_hex_len;
    Local<Value> outString ;

    int r = hash->HashDigest(&md_value, &md_len);

    if (md_len == 0 || r == 0) {
      return scope.Close(String::New(""));
    }

    if (args.Length() == 0 || !args[0]->IsString()) {
      // Binary
      outString = Encode(md_value, md_len, BINARY);
    } else {
      String::Utf8Value encoding(args[0]->ToString());
      if (strcasecmp(*encoding, "hex") == 0) {
        // Hex encoding
        hex_encode(md_value, md_len, &md_hexdigest, &md_hex_len);
        outString = Encode(md_hexdigest, md_hex_len, BINARY);
        free(md_hexdigest);
      } else if (strcasecmp(*encoding, "base64") == 0) {
        base64(md_value, md_len, &md_hexdigest, &md_hex_len);
        outString = Encode(md_hexdigest, md_hex_len, BINARY);
        free(md_hexdigest);
      } else if (strcasecmp(*encoding, "binary") == 0) {
        outString = Encode(md_value, md_len, BINARY);
      } else {
	fprintf(stderr, "node-crypto : Hash .digest encoding "
		"can be binary, hex or base64\n");
      }
    }
    free(md_value);
    return scope.Close(outString);

  }

  Hash () : ObjectWrap () 
  {
    initialised = false;
  }

  ~Hash ()
  {
  }

 private:

  EVP_MD_CTX mdctx;
  const EVP_MD *md;
  bool initialised;

};

class Sign : public ObjectWrap {
 public:
  static void
  Initialize (v8::Handle<v8::Object> target)
  {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    t->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(t, "init", SignInit);
    NODE_SET_PROTOTYPE_METHOD(t, "update", SignUpdate);
    NODE_SET_PROTOTYPE_METHOD(t, "sign", SignFinal);

    target->Set(String::NewSymbol("Sign"), t->GetFunction());
  }

  bool SignInit (const char* signType)
  {
    md = EVP_get_digestbyname(signType);
    if(!md) {
      printf("Unknown message digest %s\n", signType);
      return false;
    }
    EVP_MD_CTX_init(&mdctx);
    EVP_SignInit_ex(&mdctx, md, NULL);
    initialised = true;
    return true;
    
  }

  int SignUpdate(char* data, int len) {
    if (!initialised)
      return 0;
    EVP_SignUpdate(&mdctx, data, len);
    return 1;
  }

  int SignFinal(unsigned char** md_value, unsigned int *md_len, char* keyPem, int keyPemLen) {
    if (!initialised)
      return 0;

    BIO *bp = NULL;
    EVP_PKEY* pkey;
    bp = BIO_new(BIO_s_mem()); 
    if(!BIO_write(bp, keyPem, keyPemLen))
      return 0;

    pkey = PEM_read_bio_PrivateKey( bp, NULL, NULL, NULL );
    if (pkey == NULL)
      return 0;

    EVP_SignFinal(&mdctx, *md_value, md_len, pkey);
    EVP_MD_CTX_cleanup(&mdctx);
    initialised = false;
    EVP_PKEY_free(pkey);
    BIO_free(bp);
    return 1;
  }


 protected:

  static Handle<Value>
  New (const Arguments& args)
  {
    HandleScope scope;

    Sign *sign = new Sign();
    sign->Wrap(args.This());

    return args.This();
  }

  static Handle<Value>
  SignInit(const Arguments& args) {
    Sign *sign = ObjectWrap::Unwrap<Sign>(args.This());

    HandleScope scope;

    if (args.Length() == 0 || !args[0]->IsString()) {
      return ThrowException(String::New("Must give signtype string as argument"));
    }

    String::Utf8Value signType(args[0]->ToString());

    bool r = sign->SignInit(*signType);

    return args.This();
  }

  static Handle<Value>
  SignUpdate(const Arguments& args) {
    Sign *sign = ObjectWrap::Unwrap<Sign>(args.This());

    HandleScope scope;

    enum encoding enc = ParseEncoding(args[1]);
    ssize_t len = DecodeBytes(args[0], enc);

    if (len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* buf = new char[len];
    ssize_t written = DecodeWrite(buf, len, args[0], enc);
    assert(written == len);

    int r = sign->SignUpdate(buf, len);

    return args.This();
  }

  static Handle<Value>
  SignFinal(const Arguments& args) {
    Sign *sign = ObjectWrap::Unwrap<Sign>(args.This());

    HandleScope scope;

    unsigned char* md_value;
    unsigned int md_len;
    char* md_hexdigest;
    int md_hex_len;
    Local<Value> outString;

    md_len = 8192; // Maximum key size is 8192 bits
    md_value = new unsigned char[md_len];

    ssize_t len = DecodeBytes(args[0], BINARY);

    if (len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* buf = new char[len];
    ssize_t written = DecodeWrite(buf, len, args[0], BINARY);
    assert(written == len);


    int r = sign->SignFinal(&md_value, &md_len, buf, len);

    if (md_len == 0 || r == 0) {
      return scope.Close(String::New(""));
    }

    if (args.Length() == 1 || !args[1]->IsString()) {
      // Binary
      outString = Encode(md_value, md_len, BINARY);
    } else {
      String::Utf8Value encoding(args[1]->ToString());
      if (strcasecmp(*encoding, "hex") == 0) {
        // Hex encoding
        hex_encode(md_value, md_len, &md_hexdigest, &md_hex_len);
        outString = Encode(md_hexdigest, md_hex_len, BINARY);
        free(md_hexdigest);
      } else if (strcasecmp(*encoding, "base64") == 0) {
        base64(md_value, md_len, &md_hexdigest, &md_hex_len);
        outString = Encode(md_hexdigest, md_hex_len, BINARY);
        free(md_hexdigest);
      } else if (strcasecmp(*encoding, "binary") == 0) {
        outString = Encode(md_value, md_len, BINARY);
      } else {
	outString = String::New("");
	fprintf(stderr, "node-crypto : Sign .sign encoding "
		"can be binary, hex or base64\n");
      }
    }
    return scope.Close(outString);

  }

  Sign () : ObjectWrap () 
  {
    initialised = false;
  }

  ~Sign ()
  {
  }

 private:

  EVP_MD_CTX mdctx;
  const EVP_MD *md;
  bool initialised;

};

class Verify : public ObjectWrap {
 public:
  static void
  Initialize (v8::Handle<v8::Object> target)
  {
    HandleScope scope;

    Local<FunctionTemplate> t = FunctionTemplate::New(New);

    t->InstanceTemplate()->SetInternalFieldCount(1);

    NODE_SET_PROTOTYPE_METHOD(t, "init", VerifyInit);
    NODE_SET_PROTOTYPE_METHOD(t, "update", VerifyUpdate);
    NODE_SET_PROTOTYPE_METHOD(t, "verify", VerifyFinal);

    target->Set(String::NewSymbol("Verify"), t->GetFunction());
  }

  bool VerifyInit (const char* verifyType)
  {
    md = EVP_get_digestbyname(verifyType);
    if(!md) {
      fprintf(stderr, "node-crypto : Unknown message digest %s\n", verifyType);
      return false;
    }
    EVP_MD_CTX_init(&mdctx);
    EVP_VerifyInit_ex(&mdctx, md, NULL);
    initialised = true;
    return true;
    
  }

  int VerifyUpdate(char* data, int len) {
    if (!initialised)
      return 0;
    EVP_VerifyUpdate(&mdctx, data, len);
    return 1;
  }

  int VerifyFinal(char* keyPem, int keyPemLen, unsigned char* sig, int siglen) {
    if (!initialised)
      return 0;

    BIO *bp = NULL;
    EVP_PKEY* pkey;
    X509 *        x509;

    bp = BIO_new(BIO_s_mem()); 
    if(!BIO_write(bp, keyPem, keyPemLen))
      return 0;

    x509 = PEM_read_bio_X509(bp, NULL, NULL, NULL );
    if (x509==NULL)
      return 0;

    pkey=X509_get_pubkey(x509);
    if (pkey==NULL)
      return 0;

    int r = EVP_VerifyFinal(&mdctx, sig, siglen, pkey);
    EVP_PKEY_free (pkey);

    if (r != 1) {
      ERR_print_errors_fp (stderr);
    }
    X509_free(x509);
    BIO_free(bp);
    EVP_MD_CTX_cleanup(&mdctx);
    initialised = false;
    return r;
  }


 protected:

  static Handle<Value>
  New (const Arguments& args)
  {
    HandleScope scope;

    Verify *verify = new Verify();
    verify->Wrap(args.This());

    return args.This();
  }

  static Handle<Value>
  VerifyInit(const Arguments& args) {
    Verify *verify = ObjectWrap::Unwrap<Verify>(args.This());

    HandleScope scope;

    if (args.Length() == 0 || !args[0]->IsString()) {
      return ThrowException(String::New("Must give verifytype string as argument"));
    }

    String::Utf8Value verifyType(args[0]->ToString());

    bool r = verify->VerifyInit(*verifyType);

    return args.This();
  }

  static Handle<Value>
  VerifyUpdate(const Arguments& args) {
    Verify *verify = ObjectWrap::Unwrap<Verify>(args.This());

    HandleScope scope;

    enum encoding enc = ParseEncoding(args[1]);
    ssize_t len = DecodeBytes(args[0], enc);

    if (len < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* buf = new char[len];
    ssize_t written = DecodeWrite(buf, len, args[0], enc);
    assert(written == len);

    int r = verify->VerifyUpdate(buf, len);

    return args.This();
  }

  static Handle<Value>
  VerifyFinal(const Arguments& args) {
    Verify *verify = ObjectWrap::Unwrap<Verify>(args.This());

    HandleScope scope;

    ssize_t klen = DecodeBytes(args[0], BINARY);

    if (klen < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }

    char* kbuf = new char[klen];
    ssize_t kwritten = DecodeWrite(kbuf, klen, args[0], BINARY);
    assert(kwritten == klen);


    ssize_t hlen = DecodeBytes(args[1], BINARY);

    if (hlen < 0) {
      Local<Value> exception = Exception::TypeError(String::New("Bad argument"));
      return ThrowException(exception);
    }
    
    unsigned char* hbuf = new unsigned char[hlen];
    ssize_t hwritten = DecodeWrite((char *)hbuf, hlen, args[1], BINARY);
    assert(hwritten == hlen);
    unsigned char* dbuf;
    int dlen;

    int r=-1;

    if (args.Length() == 2 || !args[2]->IsString()) {
      // Binary
      r = verify->VerifyFinal(kbuf, klen, hbuf, hlen);
    } else {
      String::Utf8Value encoding(args[2]->ToString());
      if (strcasecmp(*encoding, "hex") == 0) {
        // Hex encoding
        hex_decode(hbuf, hlen, (char **)&dbuf, &dlen);
        r = verify->VerifyFinal(kbuf, klen, dbuf, dlen);
	free(dbuf);
      } else if (strcasecmp(*encoding, "base64") == 0) {
        // Base64 encoding
        unbase64(hbuf, hlen, (char **)&dbuf, &dlen);
        r = verify->VerifyFinal(kbuf, klen, dbuf, dlen);
	free(dbuf);
      } else if (strcasecmp(*encoding, "binary") == 0) {
        r = verify->VerifyFinal(kbuf, klen, hbuf, hlen);
      } else {
	fprintf(stderr, "node-crypto : Verify .verify encoding "
		"can be binary, hex or base64\n");
      }
    }

    return scope.Close(Integer::New(r));
  }

  Verify () : ObjectWrap () 
  {
    initialised = false;
  }

  ~Verify ()
  {
  }

 private:

  EVP_MD_CTX mdctx;
  const EVP_MD *md;
  bool initialised;

};



extern "C" void
init (Handle<Object> target) 
{
  HandleScope scope;

  ERR_load_crypto_strings();
  OpenSSL_add_all_digests();
  OpenSSL_add_all_algorithms();

  Cipher::Initialize(target);
  Decipher::Initialize(target);
  Hmac::Initialize(target);
  Hash::Initialize(target);
  Sign::Initialize(target);
  Verify::Initialize(target);
}

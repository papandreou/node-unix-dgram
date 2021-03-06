// -D_GNU_SOURCE makes SOCK_NONBLOCK etc. available on linux
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <node.h>
#include <node_buffer.h>

#include <map>

using namespace v8;
using namespace node;

namespace {

typedef std::map<int, ev_io*> watchers_t;

struct SocketContext {
  Persistent<Function> cb_;
  int fd_;
};


Persistent<String> errno_symbol;
watchers_t watchers;


void SetNonBlock(int fd) {
  int flags;
  int r;

  flags = fcntl(fd, F_GETFL);
  assert(flags != -1);

  r = fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  assert(r != -1);
}


void SetCloExec(int fd) {
  int flags;
  int r;

  flags = fcntl(fd, F_GETFD);
  assert(flags != -1);

  r = fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  assert(r != -1);
}


void SetErrno(int errorno) {
  // set errno in the global context, this is the technique
  // that node uses to propagate system level errors to JS land
  Context::GetCurrent()->Global()->Set(errno_symbol, Integer::New(errorno));
}


void OnRecv(EV_P_ ev_io* w, int revents) {
  HandleScope scope;
  Handle<Value> argv[3];
  sockaddr_storage ss;
  SocketContext* sc;
  Buffer* buf;
  msghdr msg;
  iovec iov;
  int size;
  int r;

  r = -1;
  buf = NULL;
  argv[0] = argv[1] = argv[2] = Null();

  assert(!(revents & ~EV_READ));

  sc = reinterpret_cast<SocketContext*>(w->data);
  assert(sc != NULL);

  if ((r = ioctl(sc->fd_, FIONREAD, &size)) == -1) {
    SetErrno(errno);
    goto err;
  }

  buf = Buffer::New(size);
  argv[1] = buf->handle_;

  iov.iov_base = Buffer::Data(buf);
  iov.iov_len = size;

  memset(&msg, 0, sizeof msg);
  msg.msg_iovlen = 1;
  msg.msg_iov = &iov;
  msg.msg_name = &ss;
  msg.msg_namelen = sizeof ss;

  if ((r = recvmsg(sc->fd_, &msg, 0)) == -1) {
    SetErrno(errno);
    goto err;
  }

err:
  argv[0] = Integer::New(r);

  TryCatch tc;

  sc->cb_->Call(Context::GetCurrent()->Global(),
                sizeof(argv) / sizeof(argv[0]),
                argv);

  if (tc.HasCaught())
    FatalException(tc);
}


void StartWatcher(int fd, Handle<Value> callback) {
  // start listening for incoming dgrams
  SocketContext* sc = new SocketContext;
  sc->cb_ = Persistent<Function>::New(callback.As<Function>());
  sc->fd_ = fd;

  ev_io* w = new ev_io;
  ev_io_init(w, OnRecv, fd, EV_READ);
  w->data = reinterpret_cast<void*>(sc);
  ev_io_start(EV_DEFAULT_UC_ w);

  // so we can disarm the watcher when close(fd) is called
  watchers.insert(watchers_t::value_type(fd, w));
}


void StopWatcher(int fd) {
  watchers_t::iterator iter = watchers.find(fd);
  assert(iter != watchers.end());

  ev_io* w = iter->second;
  SocketContext* sc = reinterpret_cast<SocketContext*>(w->data);
  sc->cb_.Dispose();
  sc->cb_.Clear();

  ev_io_stop(EV_DEFAULT_UC_ w);
  watchers.erase(iter);

  delete sc;
  delete w;
}


Handle<Value> Socket(const Arguments& args) {
  HandleScope scope;
  Local<Value> cb;
  int protocol;
  int domain;
  int type;
  int fd;

  assert(args.Length() == 4);

  domain    = args[0]->Int32Value();
  type      = args[1]->Int32Value();
  protocol  = args[2]->Int32Value();
  cb        = args[3];

#if defined(SOCK_NONBLOCK)
  type |= SOCK_NONBLOCK;
#endif
#if defined(SOCK_CLOEXEC)
  type |= SOCK_CLOEXEC;
#endif

  if ((fd = socket(domain, type, protocol)) == -1) {
    SetErrno(errno);
    goto out;
  }

  #if !defined(SOCK_NONBLOCK)
  SetNonBlock(fd);
#endif
#if !defined(SOCK_CLOEXEC)
  SetCloExec(fd);
#endif

  StartWatcher(fd, cb);

out:
  return scope.Close(Integer::New(fd));
}


Handle<Value> Bind(const Arguments& args) {
  HandleScope scope;
  sockaddr_un sun;
  int fd;
  int r;

  assert(args.Length() == 2);

  fd = args[0]->Int32Value();
  String::Utf8Value path(args[1]);

  strncpy(sun.sun_path, *path, sizeof(sun.sun_path) - 1);
  sun.sun_path[sizeof(sun.sun_path) - 1] = '\0';
  sun.sun_family = AF_UNIX;

  if ((r = bind(fd, reinterpret_cast<sockaddr*>(&sun), sizeof sun)) == -1)
    SetErrno(errno);

  return scope.Close(Integer::New(r));
}


Handle<Value> Send(const Arguments& args) {
  HandleScope scope;
  Local<Object> buf;
  sockaddr_un sun;
  size_t offset;
  size_t length;
  msghdr msg;
  iovec iov;
  int fd;
  int r;

  assert(args.Length() == 5);

  fd = args[0]->Int32Value();
  buf = args[1]->ToObject();
  offset = args[2]->Uint32Value();
  length = args[3]->Uint32Value();
  String::Utf8Value path(args[4]);

  assert(Buffer::HasInstance(buf));
  assert(offset + length <= Buffer::Length(buf));

  iov.iov_base = Buffer::Data(buf) + offset;
  iov.iov_len = length;

  strncpy(sun.sun_path, *path, sizeof(sun.sun_path) - 1);
  sun.sun_path[sizeof(sun.sun_path) - 1] = '\0';
  sun.sun_family = AF_UNIX;

  memset(&msg, 0, sizeof msg);
  msg.msg_iovlen = 1;
  msg.msg_iov = &iov;
  msg.msg_name = reinterpret_cast<void*>(&sun);
  msg.msg_namelen = sizeof sun;

  if ((r = sendmsg(fd, &msg, 0)) == -1)
    SetErrno(errno);

  return scope.Close(Integer::New(r));
}


Handle<Value> Close(const Arguments& args) {
  HandleScope scope;
  int fd;
  int r;

  assert(args.Length() == 1);

  fd = args[0]->Int32Value();
  do {
    r = close(fd);
  }
  while (r == -1 && errno == EINTR);

  if (r)
    SetErrno(errno);

  StopWatcher(fd);

  return scope.Close(Integer::New(r));
}


void Initialize(Handle<Object> target) {
  errno_symbol = Persistent<String>::New(String::NewSymbol("errno"));

  // don't need to be read-only, only used by the JS shim
  target->Set(String::NewSymbol("AF_UNIX"), Integer::New(AF_UNIX));
  target->Set(String::NewSymbol("SOCK_DGRAM"), Integer::New(SOCK_DGRAM));

  target->Set(String::NewSymbol("socket"),
              FunctionTemplate::New(Socket)->GetFunction());

  target->Set(String::NewSymbol("bind"),
              FunctionTemplate::New(Bind)->GetFunction());

  target->Set(String::NewSymbol("send"),
              FunctionTemplate::New(Send)->GetFunction());

  target->Set(String::NewSymbol("close"),
              FunctionTemplate::New(Close)->GetFunction());
}


} // anonymous namespace

NODE_MODULE(unix_dgram, Initialize)

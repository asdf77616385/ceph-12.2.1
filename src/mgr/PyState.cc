// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2016 John Spray <john.spray@redhat.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 */

/**
 * The interface we present to python code that runs within
 * ceph-mgr.
 */

#include "Mgr.h"

#include "mon/MonClient.h"
#include "common/errno.h"
#include "common/version.h"

#include "PyState.h"
#include "Gil.h"

#define dout_context g_ceph_context
#define dout_subsys ceph_subsys_mgr

PyModules *global_handle = NULL;


class MonCommandCompletion : public Context
{
  PyObject *python_completion;
  const std::string tag;
  PyThreadState *pThreadState;

public:
  std::string outs;
  bufferlist outbl;

  MonCommandCompletion(PyObject* ev, const std::string &tag_, PyThreadState *ts_)
    : python_completion(ev), tag(tag_), pThreadState(ts_)
  {
    assert(python_completion != nullptr);
    Py_INCREF(python_completion);
  }

  ~MonCommandCompletion() override
  {
    Py_DECREF(python_completion);
  }

  void finish(int r) override
  {
    dout(10) << "MonCommandCompletion::finish()" << dendl;
    {
      // Scoped so the Gil is released before calling notify_all()
      // Create new thread state because this is called via the MonClient
      // Finisher, not the PyModules finisher.
      Gil gil(pThreadState, true);

      auto set_fn = PyObject_GetAttrString(python_completion, "complete");
      assert(set_fn != nullptr);

      auto pyR = PyInt_FromLong(r);
      auto pyOutBl = PyString_FromString(outbl.to_str().c_str());
      auto pyOutS = PyString_FromString(outs.c_str());
      auto args = PyTuple_Pack(3, pyR, pyOutBl, pyOutS);
      Py_DECREF(pyR);
      Py_DECREF(pyOutBl);
      Py_DECREF(pyOutS);

      auto rtn = PyObject_CallObject(set_fn, args);
      if (rtn != nullptr) {
	Py_DECREF(rtn);
      }
      Py_DECREF(args);
    }
    global_handle->notify_all("command", tag);
  }
};


static PyObject*
ceph_send_command(PyObject *self, PyObject *args)
{
  char *handle = nullptr;

  // Like mon, osd, mds
  char *type = nullptr;

  // Like "23" for an OSD or "myid" for an MDS
  char *name = nullptr;

  char *cmd_json = nullptr;
  char *tag = nullptr;
  PyObject *completion = nullptr;
  if (!PyArg_ParseTuple(args, "sOssss:ceph_send_command",
        &handle, &completion, &type, &name, &cmd_json, &tag)) {
    return nullptr;
  }

  auto set_fn = PyObject_GetAttrString(completion, "complete");
  if (set_fn == nullptr) {
    ceph_abort();  // TODO raise python exception instead
  } else {
    assert(PyCallable_Check(set_fn));
  }
  Py_DECREF(set_fn);

  auto c = new MonCommandCompletion(completion, tag, PyThreadState_Get());
  if (std::string(type) == "mon") {
    global_handle->get_monc().start_mon_command(
        {cmd_json},
        {},
        &c->outbl,
        &c->outs,
        c);
  } else if (std::string(type) == "osd") {
    std::string err;
    uint64_t osd_id = strict_strtoll(name, 10, &err);
    if (!err.empty()) {
      delete c;
      string msg("invalid osd_id: ");
      msg.append("\"").append(name).append("\"");
      PyErr_SetString(PyExc_ValueError, msg.c_str());
      return nullptr;
    }

    ceph_tid_t tid;
    global_handle->get_objecter().osd_command(
        osd_id,
        {cmd_json},
        {},
        &tid,
        &c->outbl,
        &c->outs,
        c);
  } else if (std::string(type) == "mds") {
    int r = global_handle->get_client().mds_command(
        name,
        {cmd_json},
        {},
        &c->outbl,
        &c->outs,
        c);
    if (r != 0) {
      string msg("failed to send command to mds: ");
      msg.append(cpp_strerror(r));
      PyErr_SetString(PyExc_RuntimeError, msg.c_str());
      return nullptr;
    }
  } else if (std::string(type) == "pg") {
    pg_t pgid;
    if (!pgid.parse(name)) {
      delete c;
      string msg("invalid pgid: ");
      msg.append("\"").append(name).append("\"");
      PyErr_SetString(PyExc_ValueError, msg.c_str());
      return nullptr;
    }

    ceph_tid_t tid;
    global_handle->get_objecter().pg_command(
        pgid,
        {cmd_json},
        {},
        &tid,
        &c->outbl,
        &c->outs,
        c);
    return nullptr;
  } else {
    delete c;
    string msg("unknown service type: ");
    msg.append(type);
    PyErr_SetString(PyExc_ValueError, msg.c_str());
    return nullptr;
  }

  Py_RETURN_NONE;
}

static PyObject*
ceph_set_health_checks(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  PyObject *checks = NULL;
  if (!PyArg_ParseTuple(args, "sO:ceph_set_health_checks", &handle, &checks)) {
    return NULL;
  }
  if (!PyDict_Check(checks)) {
    derr << __func__ << " arg not a dict" << dendl;
    Py_RETURN_NONE;
  }
  PyObject *checksls = PyDict_Items(checks);
  health_check_map_t out_checks;
  for (int i = 0; i < PyList_Size(checksls); ++i) {
    PyObject *kv = PyList_GET_ITEM(checksls, i);
    char *check_name = nullptr;
    PyObject *check_info = nullptr;
    if (!PyArg_ParseTuple(kv, "sO:pair", &check_name, &check_info)) {
      derr << __func__ << " dict item " << i
	   << " not a size 2 tuple" << dendl;
      continue;
    }
    if (!PyDict_Check(check_info)) {
      derr << __func__ << " item " << i << " " << check_name
	   << " value not a dict" << dendl;
      continue;
    }
    health_status_t severity = HEALTH_OK;
    string summary;
    list<string> detail;
    PyObject *infols = PyDict_Items(check_info);
    for (int j = 0; j < PyList_Size(infols); ++j) {
      PyObject *pair = PyList_GET_ITEM(infols, j);
      if (!PyTuple_Check(pair)) {
	derr << __func__ << " item " << i << " pair " << j
	     << " not a tuple" << dendl;
	continue;
      }
      char *k = nullptr;
      PyObject *v = nullptr;
      if (!PyArg_ParseTuple(pair, "sO:pair", &k, &v)) {
	derr << __func__ << " item " << i << " pair " << j
	     << " not a size 2 tuple" << dendl;
	continue;
      }
      string ks(k);
      if (ks == "severity") {
	if (!PyString_Check(v)) {
	  derr << __func__ << " check " << check_name
	       << " severity value not string" << dendl;
	  continue;
	}
	string vs(PyString_AsString(v));
	if (vs == "warning") {
	  severity = HEALTH_WARN;
	} else if (vs == "error") {
	  severity = HEALTH_ERR;
	}
      } else if (ks == "summary") {
	if (!PyString_Check(v)) {
	  derr << __func__ << " check " << check_name
	       << " summary value not string" << dendl;
	  continue;
	}
	summary = PyString_AsString(v);
      } else if (ks == "detail") {
	if (!PyList_Check(v)) {
	  derr << __func__ << " check " << check_name
	       << " detail value not list" << dendl;
	  continue;
	}
	for (int k = 0; k < PyList_Size(v); ++k) {
	  PyObject *di = PyList_GET_ITEM(v, k);
	  if (!PyString_Check(di)) {
	    derr << __func__ << " check " << check_name
		 << " detail item " << k << " not a string" << dendl;
	    continue;
	  }
	  detail.push_back(PyString_AsString(di));
	}
      } else {
	derr << __func__ << " check " << check_name
	     << " unexpected key " << k << dendl;
      }
    }
    auto& d = out_checks.add(check_name, severity, summary);
    d.detail.swap(detail);
  }

  JSONFormatter jf(true);
  dout(10) << "module " << handle << " health checks:\n";
  out_checks.dump(&jf);
  jf.flush(*_dout);
  *_dout << dendl;

  global_handle->set_health_checks(handle, std::move(out_checks));
  
  Py_RETURN_NONE;
}


static PyObject*
ceph_state_get(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  char *what = NULL;
  if (!PyArg_ParseTuple(args, "ss:ceph_state_get", &handle, &what)) {
    return NULL;
  }

  return global_handle->get_python(what);
}


static PyObject*
ceph_get_server(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  char *hostname = NULL;
  if (!PyArg_ParseTuple(args, "sz:ceph_get_server", &handle, &hostname)) {
    return NULL;
  }

  if (hostname) {
    return global_handle->get_server_python(hostname);
  } else {
    return global_handle->list_servers_python();
  }
}

static PyObject*
ceph_get_mgr_id(PyObject *self, PyObject *args)
{
  return PyString_FromString(g_conf->name.get_id().c_str());
}

static PyObject*
ceph_config_get(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  char *what = nullptr;
  if (!PyArg_ParseTuple(args, "ss:ceph_config_get", &handle, &what)) {
    derr << "Invalid args!" << dendl;
    return nullptr;
  }

  std::string value;
  bool found = global_handle->get_config(handle, what, &value);
  if (found) {
    dout(10) << "ceph_config_get " << what << " found: " << value.c_str() << dendl;
    return PyString_FromString(value.c_str());
  } else {
    dout(4) << "ceph_config_get " << what << " not found " << dendl;
    Py_RETURN_NONE;
  }
}

static PyObject*
ceph_config_get_prefix(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  char *prefix = nullptr;
  if (!PyArg_ParseTuple(args, "ss:ceph_config_get", &handle, &prefix)) {
    derr << "Invalid args!" << dendl;
    return nullptr;
  }

  return global_handle->get_config_prefix(handle, prefix);
}

static PyObject*
ceph_config_set(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  char *key = nullptr;
  char *value = nullptr;
  if (!PyArg_ParseTuple(args, "ssz:ceph_config_set", &handle, &key, &value)) {
    return nullptr;
  }
  boost::optional<string> val;
  if (value) {
    val = value;
  }
  global_handle->set_config(handle, key, val);

  Py_RETURN_NONE;
}

static PyObject*
get_metadata(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  char *svc_name = NULL;
  char *svc_id = NULL;
  if (!PyArg_ParseTuple(args, "sss:get_metadata", &handle, &svc_name, &svc_id)) {
    return nullptr;
  }
  return global_handle->get_metadata_python(handle, svc_name, svc_id);
}

static PyObject*
get_daemon_status(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  char *svc_name = NULL;
  char *svc_id = NULL;
  if (!PyArg_ParseTuple(args, "sss:get_daemon_status", &handle, &svc_name,
			&svc_id)) {
    return nullptr;
  }
  return global_handle->get_daemon_status_python(handle, svc_name, svc_id);
}

static PyObject*
ceph_log(PyObject *self, PyObject *args)
{
  int level = 0;
  char *record = nullptr;
  char *handle = nullptr;
  if (!PyArg_ParseTuple(args, "sis:log", &handle, &level, &record)) {
    return nullptr;
  }

  global_handle->log(handle, level, record);

  Py_RETURN_NONE;
}

static PyObject *
ceph_get_version(PyObject *self, PyObject *args)
{
  return PyString_FromString(pretty_version_to_str().c_str());
}

static PyObject *
ceph_get_context(PyObject *self, PyObject *args)
{
  return global_handle->get_context();
}

static PyObject*
get_counter(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  char *svc_name = nullptr;
  char *svc_id = nullptr;
  char *counter_path = nullptr;
  if (!PyArg_ParseTuple(args, "ssss:get_counter", &handle, &svc_name,
                                                  &svc_id, &counter_path)) {
    return nullptr;
  }
  return global_handle->get_counter_python(
      handle, svc_name, svc_id, counter_path);
}

static PyObject*
get_perf_schema(PyObject *self, PyObject *args)
{
  char *handle = nullptr;
  char *type_str = nullptr;
  char *svc_id = nullptr;
  if (!PyArg_ParseTuple(args, "sss:get_perf_schema", &handle, &type_str,
                                                     &svc_id)) {
    return nullptr;
  }

  return global_handle->get_perf_schema_python(handle, type_str, svc_id);
}

PyMethodDef CephStateMethods[] = {
    {"get", ceph_state_get, METH_VARARGS,
     "Get a cluster object"},
    {"get_server", ceph_get_server, METH_VARARGS,
     "Get a server object"},
    {"get_metadata", get_metadata, METH_VARARGS,
     "Get a service's metadata"},
    {"get_daemon_status", get_daemon_status, METH_VARARGS,
     "Get a service's status"},
    {"send_command", ceph_send_command, METH_VARARGS,
     "Send a mon command"},
    {"set_health_checks", ceph_set_health_checks, METH_VARARGS,
     "Set health checks for this module"},
    {"get_mgr_id", ceph_get_mgr_id, METH_NOARGS,
     "Get the mgr id"},
    {"get_config", ceph_config_get, METH_VARARGS,
     "Get a configuration value"},
    {"get_config_prefix", ceph_config_get_prefix, METH_VARARGS,
     "Get all configuration values with a given prefix"},
    {"set_config", ceph_config_set, METH_VARARGS,
     "Set a configuration value"},
    {"get_counter", get_counter, METH_VARARGS,
      "Get a performance counter"},
    {"get_perf_schema", get_perf_schema, METH_VARARGS,
      "Get the performance counter schema"},
    {"log", ceph_log, METH_VARARGS,
     "Emit a (local) log message"},
    {"get_version", ceph_get_version, METH_VARARGS,
     "Get the ceph version of this process"},
    {"get_context", ceph_get_context, METH_NOARGS,
      "Get a CephContext* in a python capsule"},
    {NULL, NULL, 0, NULL}
};


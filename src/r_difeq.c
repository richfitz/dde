#include "r_difeq.h"
#include "difeq.h"

SEXP difeq_ptr_create(difeq_data *obj);
void difeq_ptr_finalizer(SEXP extPtr);

SEXP r_difeq(SEXP r_y_initial, SEXP r_steps, SEXP r_target, SEXP r_data,
             SEXP r_n_out,
             SEXP r_t0, SEXP r_dt,
             SEXP r_data_is_real,
             SEXP r_n_history, SEXP r_return_history,
             SEXP r_return_initial) {
  double *y_initial = REAL(r_y_initial);
  size_t n = length(r_y_initial);

  size_t n_steps = LENGTH(r_steps);
  size_t *steps = (size_t*) R_alloc(n_steps, sizeof(size_t));
  int* tmp = INTEGER(r_steps);
  for (size_t i = 0; i < n_steps; ++i) {
    steps[i] = (size_t) tmp[i];
  }

  double t0 = REAL(r_t0)[0], dt = REAL(r_dt)[0];

  // TODO: check for NULL function pointers here to avoid crashes;
  // also test type?
  difeq_target *target = (difeq_target*)R_ExternalPtrAddr(r_target);
  void *data = NULL;
  if (TYPEOF(r_data) == REALSXP && INTEGER(r_data_is_real)[0]) {
    data = (void*) REAL(r_data);
  } else if (TYPEOF(r_data) == EXTPTRSXP) {
    data = R_ExternalPtrAddr(r_data);
  } else {
    data = (void*) r_data;
  }

  size_t n_history = (size_t)INTEGER(r_n_history)[0];
  bool return_history = INTEGER(r_return_history)[0];
  bool return_initial = INTEGER(r_return_initial)[0];
  size_t nt = return_initial ? n_steps : n_steps - 1;

  size_t n_out = INTEGER(r_n_out)[0];
  double *out = NULL;
  SEXP r_out = R_NilValue;
  if (n_out > 0) {
    r_out = PROTECT(allocMatrix(REALSXP, n_out, nt));
    out = REAL(r_out);
  }

  // TODO: as an option save the conditions here.  That's not too bad
  // because we just don't pass through REAL(r_y) but REAL(r_y) +
  // n.  We do have to run the output functions once more though.
  difeq_data* obj = difeq_data_alloc(target, n, n_out, data, n_history);

  // This is to prevent leaks in case of early exit.  If we don't make
  // it to the end of the function (for any reason, including an error
  // call in a user function, etc) R will clean up for us once it
  // garbage collects ptr.  Because R resets the protection stack on
  // early exit it is guaranteed to get collected at some point.
  SEXP ptr = PROTECT(difeq_ptr_create(obj));

  // Then solve things:
  SEXP r_y = PROTECT(allocMatrix(REALSXP, n, nt));
  double *y = REAL(r_y);

  GetRNGstate();
  difeq_run(obj, y_initial, steps, n_steps, t0, dt, y, out, return_initial);
  PutRNGstate();

  if (return_history) {
    size_t nh = ring_buffer_used(obj->history, 0);
    SEXP history = PROTECT(allocMatrix(REALSXP, obj->history_len, nh));
    ring_buffer_take(obj->history, REAL(history), nh);
    setAttrib(history, install("n"), ScalarInteger(obj->n));
    setAttrib(r_y, install("history"), history);
    UNPROTECT(1);
  }

  if (n_out > 0) {
    setAttrib(r_y, install("output"), r_out);
    UNPROTECT(1);
  }

  // Deterministically clean up if we can, otherwise we clean up by R
  // running the finaliser for us when it garbage collects ptr above.
  difeq_data_free(obj);
  R_ClearExternalPtr(ptr);
  UNPROTECT(2);
  return r_y;
}

SEXP r_yprev(SEXP r_i, SEXP r_idx) {
  size_t n = get_current_problem_size_difeq();
  if (n == 0) {
    Rf_error("Can't call this without being in an integration");
  }
  int step;
  if (TYPEOF(r_i) == INTSXP) {
    step = INTEGER(r_i)[0];
  } else if (TYPEOF(r_i) == REALSXP) {
    step = REAL(r_i)[0];
  } else {
    Rf_error("Invalid type in lag");
  }
  SEXP r_y;
  if (r_idx == R_NilValue) {
    r_y = PROTECT(allocVector(REALSXP, n));
    yprev_all(step, REAL(r_y));
  } else {
    const size_t ni = length(r_idx);
    r_y = PROTECT(allocVector(REALSXP, ni));
    if (ni == 1) {
      REAL(r_y)[0] = yprev_1(step, INTEGER(r_idx)[0] - 1);
    } else {
      r_y = allocVector(REALSXP, ni);
      size_t *idx = (size_t*) R_alloc(ni, sizeof(size_t));
      for (size_t i = 0; i < ni; ++i) {
        idx[i] = (size_t)INTEGER(r_idx)[i] - 1;
      }
      yprev_vec(step, idx, ni, REAL(r_y));
    }
  }
  UNPROTECT(1);
  return r_y;
}

void difeq_r_harness(size_t n, size_t i, double t,
                     double *y,  double *ynext,
                     size_t n_out, double *output, void *data) {
  SEXP d = (SEXP)data;
  SEXP
    target = VECTOR_ELT(d, 0),
    parms = VECTOR_ELT(d, 1),
    rho = VECTOR_ELT(d, 2);
  SEXP r_i = PROTECT(ScalarInteger(i));
  SEXP r_t = PROTECT(ScalarReal(t));
  SEXP r_y = PROTECT(allocVector(REALSXP, n));
  memcpy(REAL(r_y), y, n * sizeof(double));
  SEXP call = PROTECT(lang5(target, r_i, r_t, r_y, parms));
  SEXP ans = PROTECT(eval(call, rho));
  memcpy(ynext, REAL(ans), n * sizeof(double));
  if (n_out > 0) {
    SEXP r_output = getAttrib(ans, install("output"));
    if (r_output == R_NilValue) {
      Rf_error("Missing output");
    } else if ((size_t)length(r_output) != n_out) {
      Rf_error("Incorrect length output: expected %d, recieved %d",
               n_out, length(r_output));
    } else if (TYPEOF(r_output) != REALSXP) {
      Rf_error("Incorrect type output");
    }
    memcpy(output, REAL(r_output), n_out * sizeof(double));
  }
  UNPROTECT(5);
}

void difeq_ptr_finalizer(SEXP r_ptr) {
  void *obj = R_ExternalPtrAddr(r_ptr);
  if (obj) {
    difeq_data_free((difeq_data*) obj);
    R_ClearExternalPtr(r_ptr);
  }
}

SEXP difeq_ptr_create(difeq_data *obj) {
  SEXP r_ptr = PROTECT(R_MakeExternalPtr(obj, R_NilValue, R_NilValue));
  R_RegisterCFinalizer(r_ptr, difeq_ptr_finalizer);
  UNPROTECT(1);
  return r_ptr;
}

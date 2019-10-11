#include "osqp.h"
#include "lin_alg.h"
#include "algebra_impl.h"
#include "csc_math.h"
#include "csc_utils.h"
#include "algebra_vector.h"
#include "mkl.h"

#ifdef DFLOAT
  #define cscmv mkl_scscmv
  #define scal cblas_sscal
#else
  #define cscmv mkl_dcscmv
  #define scal cblas_dscal
#endif //float or double

#define AxpyNNZfilter 20 // Sets the threshold where mkl_Axpy is faster than the csc version

/*  logical test functions ----------------------------------------------------*/

c_int OSQPMatrix_is_eq(OSQPMatrix *A, OSQPMatrix* B, c_float tol){
  return (A->symmetry == B->symmetry &&
          csc_is_eq(A->csc, B->csc, tol) );
}


/*  Non-embeddable functions (using malloc) ----------------------------------*/

#ifndef EMBEDDED

//Make a copy from a csc matrix.  Returns OSQP_NULL on failure

OSQPMatrix* OSQPMatrix_new_from_csc(const csc* A, c_int is_triu){ /* MKL BLAS LEVEL 2 set-up */

  OSQPMatrix* out = c_malloc(sizeof(OSQPMatrix));
  if(!out) return OSQP_NULL;

  if(is_triu){
    out->symmetry = TRIU;
    out->matdescra[0] = 's'; // Setting internal MKL flag to symmetric
  }
  else{
   out->symmetry = NONE;
   out->matdescra[0] = 'g';
  }

  out->csc = csc_copy(A);

  /* Populating the MKL specifications of the matrix */
  out->matdescra[1] = 'u'; // upper triangular
  out->matdescra[2] = 'n'; // non-unit values
  out->matdescra[3] = 'c'; // zero-indexing

  if(!out->csc){
    c_free(out);
    return OSQP_NULL;
  }
  else{
    return out;
  }
}

#endif //EMBEDDED

/*  direct data access functions ---------------------------------------------*/

void OSQPMatrix_update_values(OSQPMatrix  *M,
                            const c_float *Mx_new,
                            const c_int   *Mx_new_idx,
                            c_int          M_new_n){
  csc_update_values(M->csc, Mx_new, Mx_new_idx, M_new_n);
}

/* Matrix dimensions and data access */
c_int    OSQPMatrix_get_m(const OSQPMatrix *M){return M->csc->m;}
c_int    OSQPMatrix_get_n(const OSQPMatrix *M){return M->csc->n;}
c_float* OSQPMatrix_get_x(const OSQPMatrix *M){return M->csc->x;}
c_int*   OSQPMatrix_get_i(const OSQPMatrix *M){return M->csc->i;}
c_int*   OSQPMatrix_get_p(const OSQPMatrix *M){return M->csc->p;}
c_int    OSQPMatrix_get_nz(const OSQPMatrix *M){return M->csc->p[M->csc->n];}


/* math functions ----------------------------------------------------------*/

//A = sc*A
void OSQPMatrix_mult_scalar(OSQPMatrix *A, c_float sc){ /* MKL BLAS LEVEL 2 */
  MKL_INT length = A->csc->nzmax;
  scal(length, sc, A->csc->x, 1);
}

void OSQPMatrix_lmult_diag(OSQPMatrix *A, const OSQPVectorf *L) {
  csc_lmult_diag(A->csc, OSQPVectorf_data(L));
}

void OSQPMatrix_rmult_diag(OSQPMatrix *A, const OSQPVectorf *R) {
  csc_rmult_diag(A->csc, OSQPVectorf_data(R));
}

//y = alpha*A*x + beta*y
void OSQPMatrix_Axpy(const OSQPMatrix 	  *A, 
                     const OSQPVectorf    *x,
                     OSQPVectorf          *y,
                     c_float           alpha,
                     c_float            beta) {/* MKL BLAS LEVEL 2 */
  char transa = 'n';
  const c_float* xf = OSQPVectorf_data(x);
  c_float* yf = OSQPVectorf_data(y);
  csc* Acsc = A->csc; // Dereferencing the structure
  const MKL_INT m = Acsc->m; // rows
  const MKL_INT k = Acsc->n; // columns
  const c_float* val = Acsc->x; // numerical values
  const MKL_INT* indx = Acsc->i; // row indices
  const MKL_INT* pntrb = (Acsc->p); // column pointer starting with zero
  const MKL_INT* pntre = (Acsc->p + 1); // column pointer ending with 'k' (number of columns)
  c_int nnz = Acsc->nzmax;
  if (nnz > AxpyNNZfilter ){
    if (A->symmetry == NONE){ // Use MKL
      cscmv (&transa , &m , &k , &alpha , A->matdescra , val , indx , pntrb , pntre , xf , &beta , yf );
    }
    else{
      cscmv (&transa , &m , &k , &alpha , A->matdescra , val , indx , pntrb , pntre , xf , &beta , yf );
    } 
  }
  else{ // Use csc version
    if(A->symmetry == NONE){
    //full matrix
    csc_Axpy(A->csc, xf, yf, alpha, beta);
    }
    else{
    //should be TRIU here, but not directly checked
    csc_Axpy_sym_triu(A->csc, xf, yf, alpha, beta);
    }
  }
} 

void OSQPMatrix_Atxpy(const OSQPMatrix 	   *A,
                      const OSQPVectorf    *x,
                      OSQPVectorf          *y,
                      c_float           alpha,
                      c_float            beta) {/* MKL BLAS LEVEL 2 */
  char transa = 't'; // transpose MKL flag 
  const c_float* xf = OSQPVectorf_data(x);
  c_float* yf = OSQPVectorf_data(y);
  csc* Acsc = A->csc; // dereferencing the structure to skip performing multiple searches
  const MKL_INT m = Acsc->m; // row
  const MKL_INT k = Acsc->n; // columns
  const c_float* val = Acsc->x; // numerical values
  const MKL_INT* indx = Acsc->i; // row indices
  const MKL_INT* pntrb = (Acsc->p); // column pointer starting with zero
  const MKL_INT* pntre = (Acsc->p + 1); // column pointer ending with 'k' (number of columns) 
  if(Acsc->nzmax > AxpyNNZfilter){
    if(A->symmetry == NONE){
    cscmv (&transa , &m , &k , &alpha , A->matdescra , val , indx , pntrb , pntre , xf , &beta , yf );
    }
    else{
    cscmv (&transa , &m , &k , &alpha , A->matdescra , val , indx , pntrb , pntre , xf , &beta , yf );
    }
  }
  else{
    if(A->symmetry == NONE) csc_Atxpy(A->csc, OSQPVectorf_data(x), OSQPVectorf_data(y), alpha, beta);
    else            csc_Axpy_sym_triu(A->csc, OSQPVectorf_data(x), OSQPVectorf_data(y), alpha, beta);
  }
}

c_float OSQPMatrix_quad_form(const OSQPMatrix* P, const OSQPVectorf* x) { /* MKL BLAS LEVEL 2 */
  if (P->symmetry == TRIU) {
    OSQPVectorf* y;
    y = OSQPVectorf_malloc(x->length);
    OSQPMatrix_Axpy(P, x, y, 1, 0); // Performing y=(P'*x)
    return 0.5*OSQPVectorf_dot_prod(y, x); // Performing y'*x = (P'*x)'*y = x'*P*x (quad form)
  }
  else {
#ifdef PRINTING
    c_eprint("quad_form matrix is not upper triangular");
#endif /* ifdef PRINTING */
    return -1.0;
  }
}

#if EMBEDDED != 1

void OSQPMatrix_col_norm_inf(const OSQPMatrix *M, OSQPVectorf *E) {
   csc_col_norm_inf(M->csc, OSQPVectorf_data(E));
}

void OSQPMatrix_row_norm_inf(const OSQPMatrix *M, OSQPVectorf *E) {
   if(M->symmetry == NONE) csc_row_norm_inf(M->csc, OSQPVectorf_data(E));
   else                    csc_row_norm_inf_sym_triu(M->csc, OSQPVectorf_data(E));
}

#endif // endef EMBEDDED

#ifndef EMBEDDED

void OSQPMatrix_free(OSQPMatrix *M){
  if (M) csc_spfree(M->csc);
  c_free(M);
}

OSQPMatrix* OSQPMatrix_submatrix_byrows(const OSQPMatrix* A, const OSQPVectori* rows){

  csc        *M;
  OSQPMatrix *out;


  if(A->symmetry == TRIU){
#ifdef PRINTING
    c_eprint("row selection not implemented for partially filled matrices");
#endif
    return OSQP_NULL;
  }


  M = csc_submatrix_byrows(A->csc, OSQPVectori_data(rows));

  if(!M) return OSQP_NULL;

  out = c_malloc(sizeof(OSQPMatrix));

  if(!out){
    csc_spfree(M);
    return OSQP_NULL;
  }

  out->symmetry = NONE;
  out->csc      = M;
  /* MKL BLAS set-up */
  out->matdescra[0] = 'g';
  out->matdescra[1] = 'u';
  out->matdescra[2] = 'n';
  out->matdescra[3] = 'c';

  return out;

}

#endif /* if EMBEDDED != 1 */
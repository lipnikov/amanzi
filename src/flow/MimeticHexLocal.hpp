#ifndef __MIMETICHEXLOCAL_H__
#define __MIMETICHEXLOCAL_H__

#include "Epetra_SerialDenseMatrix.h"
#include "Epetra_SerialDenseVector.h"

class MimeticHexLocal {

public:
  MimeticHexLocal() {}
  MimeticHexLocal(const double x_[][3]);
  ~MimeticHexLocal() {};

  void update(const double x_[][3]);

  void mass_matrix(Epetra_SerialDenseMatrix &matrix, bool invert = false) const
      { mass_matrix(matrix, 1.0, invert); }
  void mass_matrix(Epetra_SerialDenseMatrix &matrix, double K, bool invert = false) const;
  void mass_matrix(Epetra_SerialDenseMatrix &matrix, const Epetra_SerialSymDenseMatrix &K, bool invert = false) const;

  void diff_op(double, const double&, const double[], double&, double[]) const;
  void diff_op(const Epetra_SerialSymDenseMatrix&, const double&, const double[], double&, double[]) const;
  void diff_op(double, const double&, const Epetra_SerialDenseVector&, double&, Epetra_SerialDenseVector&) const;
  void diff_op(const Epetra_SerialSymDenseMatrix&, const double&, const Epetra_SerialDenseVector&, double&, Epetra_SerialDenseVector&) const;

  void GravityFlux(const double g[], double gflux[]) const;
  void CellFluxVector(double Fface[], double Fcell[]) const;

private:

  double hvol;
  double cwgt[8];
  Epetra_SerialDenseMatrix face_normal;

};

#endif

# Third-Party Notices

Spectrum Viewer bundles no third-party binaries of its own.

The plugin links the Open Ephys GUI and the `OpenEphysFFTW` common library,
both of which are distributed by the Open Ephys project and carry their own
notices. FFTW is redistributed by `OpenEphysFFTW`, not by this plugin.

DPSS taper generation uses a plugin-private symmetric-tridiagonal eigensolver,
so there is no BLAS or LAPACK dependency and no numerical library is shipped
alongside the plugin. Earlier releases bundled OpenBLAS for this purpose; that
dependency has been removed.

## Algorithmic provenance

The eigensolver in `Source/Numerics/SelectedTridiagonalEigensolver.cpp` was
written from the published descriptions of two classical algorithms. It is not
derived from, and contains no code from, any existing implementation.

- W. Barth, R. S. Martin and J. H. Wilkinson, "Calculation of the eigenvalues of
  a symmetric tridiagonal matrix by the method of bisection",
  *Numerische Mathematik* 9 (1967), 386-393.
- G. Peters and J. H. Wilkinson, "Inverse iteration, ill-conditioned equations
  and Newton's method", *SIAM Review* 21 (1979), 339-360.
- G. H. Golub and C. F. Van Loan, *Matrix Computations*, sections 8.2 and 8.4.

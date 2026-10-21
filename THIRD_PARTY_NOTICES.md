# Third-Party Notices

Spectrum Viewer ships no separate third-party binaries of its own. CustomMatPlot
source code is compiled into the plugin for power spectrum rendering.

The plugin links the Open Ephys GUI and the `OpenEphysFFTW` common library,
both of which are distributed by the Open Ephys project and carry their own
notices. FFTW is redistributed by `OpenEphysFFTW`, not by this plugin.

DPSS taper generation uses a plugin-private symmetric-tridiagonal eigensolver,
so there is no BLAS or LAPACK dependency and no numerical library is shipped
alongside the plugin. Earlier releases bundled OpenBLAS for this purpose; that
dependency has been removed.

## CustomMatPlot

Source: https://github.com/franshej/CustomMatPlot

Revision: `5486557becd7fbc5ca4bc478610429799539d850`

The following license is reproduced from the upstream `LICENSE.md`. Individual
source files also carry Copyright (c) 2022 Frans Rosencrantz notices.

```text
MIT License

Copyright (c) 2021 Frans Rosencrantz (frans.rosencrantz@gmail.com)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

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

// ===========================================================
//
// genBeta.cpp: Individual Inbreeding and Relatedness (Beta) on GWAS
//
// Copyright (C) 2016    Xiuwen Zheng
//
// This file is part of SNPRelate.
//
// SNPRelate is free software: you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 3 as published
// by the Free Software Foundation.
//
// SNPRelate is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with SNPRelate.  If not, see <http://www.gnu.org/licenses/>.


#ifndef _HEADER_IBD_BETA_
#define _HEADER_IBD_BETA_

// CoreArray library header
#include <dGenGWAS.h>
#include <dVect.h>
#include "ThreadPool.h"

// Standard library header
#include <cmath>
#include <cfloat>
#include <memory>
#include <algorithm>


namespace IBD_BETA
{

using namespace std;
using namespace CoreArray;
using namespace Vectorization;
using namespace GWAS;


// ---------------------------------------------------------------------
// Counting IBS variables for individual beta method

/// The structure of Individual Beta Estimator
struct TS_Beta
{
	C_UInt32 ibscnt;  ///< the number shared states defined in beta estimator
	C_UInt32 num;     ///< the total number of valid loci
};

class COREARRAY_DLL_LOCAL CIndivBeta
{
private:
	CdBaseWorkSpace &Space;

	size_t nBlock; /// the number of SNPs in a block, a multiple of 128
	VEC_AUTO_PTR<C_UInt8> Geno1b;  /// the genotype 1b representation
	TS_Beta *ptrBeta;

	void thread_ibs_num(size_t i, size_t n)
	{
		const size_t npack  = nBlock >> 3;
		const size_t npack2 = npack * 2;

		C_UInt8 *Base = Geno1b.Get();
		IdMatTri I = Array_Thread_MatIdx[i];
		C_Int64 N = Array_Thread_MatCnt[i];
		TS_Beta *p = ptrBeta + I.Offset();

		for (; N > 0; N--, ++I, p++)
		{
			C_UInt8 *p1 = Base + I.Row() * npack2;
			C_UInt8 *p2 = Base + I.Column() * npack2;
			ssize_t m = npack;

			if (p1 != p2)
			{
				// off-diagonal
				for (; m > 0; m-=8)
				{
					C_UInt64 g1_1 = *((C_UInt64*)p1);
					C_UInt64 g1_2 = *((C_UInt64*)(p1 + npack));
					C_UInt64 g2_1 = *((C_UInt64*)p2);
					C_UInt64 g2_2 = *((C_UInt64*)(p2 + npack));
					p1 += 8; p2 += 8;

					C_UInt64 mask = (g1_1 | ~g1_2) & (g2_1 | ~g2_2);
					C_UInt64 het  = (g1_1 ^ g1_2) | (g2_1 ^ g2_2);
					C_UInt64 ibs2 = ~(het | (g1_1 ^ g2_1));
					p->ibscnt += POPCNT_U64(het & mask) + 2*POPCNT_U64(ibs2 & mask);
					p->num    += POPCNT_U64(mask);
				}
			} else {
				// diagonal
				for (; m > 0; m-=8)
				{
					C_UInt64 g1 = *((C_UInt64*)p1);
					C_UInt64 g2 = *((C_UInt64*)(p1 + npack));
					p1 += 8;
					C_UInt64 mask = (g1 | ~g2);
					p->ibscnt += POPCNT_U64(~(g1 ^ g2) & mask);
					p->num    += POPCNT_U64(mask);
				}
			}
		}
	}

public:
	/// constructor
	CIndivBeta(CdBaseWorkSpace &space): Space(space) { }

	/// run the algorithm
	void Run(CdMatTri<TS_Beta> &IBS, int NumThread, bool verbose)
	{
		if (NumThread < 1) NumThread = 1;
		const size_t nSamp = Space.SampleNum();

		// detect the appropriate block size
		nBlock = 4 * GetOptimzedCache() / nSamp;
		nBlock = (nBlock / 128) * 128;
		if (nBlock < 256) nBlock = 256;
		if (nBlock > 65536) nBlock = 65536;
		const size_t nPack = nBlock / 8;
		if (verbose)
			Rprintf("%s    (internal increment: %d)\n", TimeToStr(), (int)nBlock);

		// initialize
		ptrBeta = IBS.Get();
		memset(ptrBeta, 0, sizeof(TS_Beta)*IBS.Size());

		// thread pool
		CThreadPoolEx<CIndivBeta> thpool(NumThread);
		Array_SplitJobs(NumThread, nSamp, Array_Thread_MatIdx,
			Array_Thread_MatCnt);

		// genotypes
		Geno1b.Reset(nSamp * nBlock / 4);
		VEC_AUTO_PTR<C_UInt8> Geno(nSamp * nBlock);

		// genotype buffer, false for no memory buffer
		CGenoReadBySNP WS(NumThread, Space, nBlock, verbose ? -1 : 0, false);

		// for-loop
		WS.Init();
		while (WS.Read(Geno.Get()))
		{
			C_UInt8 *pG = Geno.Get();
			C_UInt8 *pB = Geno1b.Get();
			for (size_t m=nSamp; m > 0; m--)
			{
				PackSNPGeno1b(pB, pB + nPack, pG, WS.Count(), nSamp, nBlock);
				pB += (nPack << 1);
				pG ++;
			}

			// using thread thpool
			thpool.BatchWork(this, &CIndivBeta::thread_ibs_num, NumThread);
			// update
			WS.ProgressForward(WS.Count());
		}
	}
};

}


extern "C"
{

using namespace IBD_BETA;

/// Compute the IBD coefficients by individual relatedness beta
COREARRAY_DLL_EXPORT SEXP gnrIBD_Beta(SEXP NumThread, SEXP _Verbose)
{
	bool verbose = SEXP_Verbose(_Verbose);
	COREARRAY_TRY

		// cache the genotype data
		CachingSNPData("Individual Beta", verbose);

		// the number of samples
		const size_t n = MCWorkingGeno.Space().SampleNum();
		// the upper-triangle IBS matrix
		CdMatTri<TS_Beta> IBS(n);
		{
			CIndivBeta Work(MCWorkingGeno.Space());
			Work.Run(IBS, Rf_asInteger(NumThread), verbose);
		}

		// output variables
		rv_ans = PROTECT(Rf_allocMatrix(REALSXP, n, n));
		double *pBeta = REAL(rv_ans);
		TS_Beta *p = IBS.Get();
		double avg = 0;

		// for-loop, average
		for (size_t i=0; i < n; i++)
		{
			pBeta[i*n + i] = double(p->ibscnt) / p->num;
			p ++;
			for (size_t j=i+1; j < n; j++, p++)
			{
				double s = (0.5 * p->ibscnt) / p->num;
				pBeta[i*n + j] = s;
				avg += s;
			}
		}

		avg /= C_Int64(n) * (n-1) / 2;
		double bt = 1.0 / (1 - avg);

		// for-loop, final update
		for (size_t i=0; i < n; i++)
		{
			pBeta[i*n + i] = (pBeta[i*n + i] - avg) * bt;
			for (size_t j=i+1; j < n; j++)
			{
				double s = (pBeta[i*n + j] - avg) * bt;
				pBeta[i*n + j] = pBeta[j*n + i] = s;
			}
		}

		if (verbose)
			Rprintf("%s    Done.\n", TimeToStr());
		UNPROTECT(1);

	COREARRAY_CATCH
}

}

#endif  /* _HEADER_IBD_BETA_ */

/*
------------------------------------------------------------------

This file is part of a plugin for the Open Ephys GUI
Copyright (C) 2019 Translational NeuroEngineering Laboratory

------------------------------------------------------------------

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
// Hello
#include "CumulativeTFR.h"
#include <cmath>


CumulativeTFR::CumulativeTFR(int ng1, int ng2, int nf, int nt, int Fs, float winLen, float stepLen, float freqStep,
	int freqStart, double fftSec, double alpha)
	: nFreqs(nf)
	, Fs(Fs)
	, stepLen(stepLen)
	, nTimes(nt)
	, nfft(int(fftSec * Fs))
	, ifftBuffer(nfft)
	, alpha(alpha)
	, freqStep(freqStep)
	, freqStart(freqStart)
	, windowLen(winLen)
	, pxys(ng1* ng2,
		vector<vector<ComplexWeightedAccum>>(nf,
			vector<ComplexWeightedAccum>(nt, ComplexWeightedAccum(alpha))))
	, powBuffer(ng1 + ng2,
		vector<vector<RealWeightedAccum>>(nf,
			vector<RealWeightedAccum>(nt, RealWeightedAccum(alpha))))
{

	std::cout << "Creating new TFR" << std::endl;
	std::cout << "PARAMS:" << std::endl;
	std::cout << "nFreqs: " << nFreqs << std::endl;
	std::cout << "Fs: " << Fs << std::endl;
	std::cout << "stepLen: " << stepLen << std::endl;
	std::cout << "nTimes: " << nTimes << std::endl;
	std::cout << "nfft: " << nfft << std::endl;
	std::cout << "alpha: " << alpha << std::endl;
	std::cout << "freqStep: " << freqStep << std::endl;
	std::cout << "freqStart: " << freqStart << std::endl;
	std::cout << "windowLen: " << windowLen << std::endl;

	std::cout << "Creating waveletArray" << std::endl;

	vector<vector<std::complex<double>>> wv(nf, vector<std::complex<double>>(nfft));

	waveletArray = wv;
	
	std::cout << "Creating spectrum buffer" << std::endl;

	vector<vector<vector<std::complex<double>>>> specBuff(ng1 + ng2,
		vector<vector<std::complex<double>>>(nf,
			vector<std::complex<double>>(nt)));

	spectrumBuffer = specBuff;

	std::cout << "Generating wavelets" << std::endl;

	// Create array of wavelets
	generateWavelet();

	// Trim time close to edge
	trimTime = windowLen / 2;

	printout = true;
}

void CumulativeTFR::addTrial(FFTWArrayType& fftBuffer, int chanIt)
{

	fftBuffer.fftReal();

	//std::cout << fftBuffer.getLength() << std::endl;

	float nWindow = Fs * windowLen;

	//// Use freqData to find generate spectrum and get power ////
	for (int freq = 0; freq < nFreqs; freq++)
	{
		powBuffer[chanIt][freq][0].reset();
		powBuffer[chanIt][freq][0].addValue(square(std::abs(fftBuffer.getAsComplex(freq))));

		if (0)
		{
			// Multiply fft data by wavelet
			for (int n = 0; n < nfft; n++)
			{
				ifftBuffer.set(n, std::imag(fftBuffer.getAsComplex(n)) * waveletArray[freq][n]);
			}

			// Inverse FFT on data multiplied by wavelet
			ifftBuffer.ifft();

			powBuffer[chanIt][freq][0].reset();

			// Loop over nfft
			for (int n = 0; n < nfft; n++)
			{
				std::complex<double> complex = std::imag(ifftBuffer.getAsComplex(n));

				complex *= sqrt(2.0 / nWindow) / double(nfft); // divide by nfft from matlab ifft
															   // sqrt(2/nWindow) from ft_specest_mtmconvol.m 

				double power = std::abs(complex);

				if (printout && freq == 1)
					std::cout << n << ": " << power << std::endl;

				//powBuffer[chanIt][freq][0].addValue(power);

			}

			if (printout)
				printout = false;
		}
		

		///Save convOutput for crss later (TO CHECK)
	    //spectrumBuffer[chanIt][freq][t] = complex;
	}
}

void CumulativeTFR::getMeanCoherence(int itX, int itY, AtomicallyShared<std::vector<double>>& coherence, int comb)
{
	AtomicScopedWritePtr<std::vector<double>> dataWriter(coherence);

	// Cross spectra
	for (int f = 0; f < nFreqs; ++f)
	{
		// Get crss from specturm of both chanX and chanY
		for (int t = 0; t < nTimes; t++)
		{
			std::complex<double> crss = spectrumBuffer[itX][f][t] * std::conj(spectrumBuffer[itY][f][t]);
			pxys[comb][f][t].addValue(crss);
		}
	}

	// Coherence
	for (int f = 0; f < nFreqs; ++f)
	{
		// compute coherence at each time
		RealAccum coh;

		for (int t = 0; t < nTimes; t++)
		{
			coh.addValue(singleCoherence(
				powBuffer[itX][f][t].getAverage(),
				powBuffer[itY][f][t].getAverage(),
				pxys[comb][f][t].getAverage()));
		}

		dataWriter->assign(f, coh.getAverage());

		if (nTimes < 2)
		{
			dataWriter->assign(f, 0);
		}
		else
		{
			dataWriter->assign(f, std::sqrt(coh.getVariance() * nTimes / (nTimes - 1)));
		}
	}

	dataWriter.pushUpdate();
}

void CumulativeTFR::getPowerForChannels(AtomicallyShared<std::vector<std::vector<float>>>& power)
{
	//std::cout << "Getting power." << std::endl;

	AtomicScopedWritePtr<std::vector<std::vector<float>>> dataWriter(power);

	int numChans = powBuffer.size();
	int numFreqs = powBuffer[0].size();
	int numTimes = powBuffer[0][0].size();

	for (int chn = 0; chn < numChans; ++chn)
	{
		for (int frq = 0; frq < numFreqs; ++frq)
		{
			dataWriter->at(chn)[frq] = (float) powBuffer[chn][frq][0].getAverage();
		}
	}

	//std::cout << "Pushing update!" << std::endl;
	dataWriter.pushUpdate();
}



// > Private Methods

double CumulativeTFR::singleCoherence(double pxx, double pyy, std::complex<double> pxy)
{
	return std::norm(pxy) / (pxx * pyy);
}


void CumulativeTFR::generateWavelet()
{
	std::vector<double> hann(nfft);
	std::vector<double> sinWave(nfft);
	std::vector<double> cosWave(nfft);

	waveletArray.resize(nFreqs);

	// Hann window

	float nSampWindow = Fs * windowLen;

	std::cout << "Wavelet frequency: " << Fs << std::endl;
	std::cout << "nSampWindow : " << nSampWindow << std::endl;
	std::cout << "nfft: " << nfft << std::endl;
	std::cout << "freqStart: " << freqStart << std::endl;
	std::cout << "freqStep: " << freqStep << std::endl;

	//std::cout << "Hann window: " << std::endl;

	for (int position = 0; position < nfft; position++)
	{
		//// Hann Window //// = sin^2(PI*n/N) where N=length of window
		// Create first half hann function
		if (position <= nSampWindow / 2)
		{
			// Shifted by one half cycle (pi/2)
			hann[position] = square(std::sin(double_Pi*position / nSampWindow + (double_Pi / 2.0)));
		}
		// Pad with zeroes
		else if (position <= (nfft - nSampWindow / 2))
		{
			hann[position] = 0;
		}
		// Finish off hann function
		else
		{
			// Move start of wave to nfft - windowSize/2
			int hannPosition = position - (nfft - nSampWindow / 2);
			hann[position] = square(std::sin(hannPosition * double_Pi / nSampWindow));
		}

		
		//std::cout << position << ": "  << hann[position] << std::endl;
	}

	// Wavelet
	float freqNormalized = freqStart;

	FFTWArrayType fftWaveletBuffer(nfft);

	//std::cout << "Sine wave at 0 " << std::endl;

	//std::cout << "Wavelet array: " << std::endl;
	for (int freq = 0; freq < nFreqs; freq++)
	{

		
		for (int position = 0; position < nfft; position++)
		{
			// Make sin and cos wave.
			sinWave[position] = std::sin(position * freqNormalized * (2 * double_Pi) / Fs);
			cosWave[position] = std::cos(position * freqNormalized * (2 * double_Pi) / Fs);
		
			//if (freq == 0)
			//{
			//	std::cout << position << ": " << sinWave[position] << std::endl;
			//}
		}
		freqNormalized += freqStep;

		//// Wavelet ////
		// Put into fft input array
		for (int position = 0; position < nfft; position++)
		{
			fftWaveletBuffer.set(position, std::complex<double>(cosWave.at(position) * hann.at(position), sinWave.at(position) * hann.at(position)));
		}

		fftWaveletBuffer.fftComplex();

		// Save fft output for use later
		for (int i = 0; i < nfft; i++)
		{
			waveletArray[freq][i] = fftWaveletBuffer.getAsComplex(i);
		}

		//std::cout << freq << ": " << waveletArray[freq][0] << std::endl;
	}
}



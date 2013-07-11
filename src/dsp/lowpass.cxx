/*
 * lowpass.cxx
 *
 *  Created on: 28 Jun 2013
 *      Author: mike
 */

#include <vector>
#include <string>
#include <fftw3.h>
#include <math.h>

#include "debug.h"
#include "lowpass.h"

/* Must be power of 2.  FIXME: Make runtime variable, add
 * power of 2 check to start()
 */
#define FIR_LENGTH 	64

LowPass::LowPass(const string &name) : DspBlock(name, "LowPass"),
	_firLength(FIR_LENGTH), spec(NULL), impulse(NULL),
	_passband(0),
	headpos(0),
	decimationCount(0),
	_decimation(0),
	_reqOutputRate(DEFAULT_SAMPLE_RATE)
{

}

LowPass::~LowPass()
{

}

void LowPass::setPassband(unsigned int hz)
{
	_passband = hz;

	if (isRunning())
		recalculate();
}

void LowPass::setDecimation(unsigned int n)
{
	if (isRunning())
		return;

	_decimation = n;
	_reqOutputRate = 0;
}

void LowPass::setOutputSampleRate(unsigned int hz)
{
	if (isRunning())
		return;

	_reqOutputRate = hz;
	_decimation = 0;
}

bool LowPass::init()
{
	/* Calculate required decimation rate based on whether the caller
	 * asked for a specific value, or for a specific output rate */
	if (_reqOutputRate > 0) {
		_outputSampleRate = _reqOutputRate;
		_decimation = inputSampleRate() / _outputSampleRate;
	} else if (_decimation > 0) {
		_outputSampleRate = inputSampleRate() / _decimation;
	} else {
		LOG_ERROR("Must specify either decimation or output rate\n");
		return false;
	}
	_outputChannels = inputChannels();

	/* Decimation must be integer */
	if (_outputSampleRate * _decimation != inputSampleRate()) {
		LOG_ERROR("Input rate must be integer multiple of output rate\n");
		return false;
	}
	decimationCount = 0;

	/* Set up FFTW for coefficient calculation */
	/* FIXME: Can probably use the r2c_1d plan for better performance */
	spec = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * _firLength);
	impulse = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * _firLength);
	p = fftwf_plan_dft_1d(_firLength, spec, impulse, FFTW_BACKWARD, FFTW_ESTIMATE);

	/* Pre-calculate window */
	/* FIXME: Using a rectangle is not really ideal! */
	window.resize(_firLength);
	for (unsigned int n = 0; n < _firLength; n++) {
		//window[n] = 1.0 / (float)_firLength; // also applies IFFT output scale factor
		// Hamming window (temp)
		window[n] = 0.54 - 0.46 * cosf(2 * M_PI * (float)n / (float)(_firLength - 1));
		window[n] /= (float)_firLength;
	}

	/* Allocate memory for taps */
	fir.resize(inputChannels());
	for (vector<vector<float> >::iterator it = fir.begin(); it != fir.end(); ++it)
		it->resize(_firLength);
	headpos = 0;

	/* Generate initial coefficients */
	recalculate();

	return true;
}

void LowPass::deinit()
{
	fftwf_destroy_plan(p);
	fftwf_cleanup();
	fftwf_free(spec);
	fftwf_free(impulse);

	/* Release vector memory */
	vector<float>().swap(window);
	vector<float>().swap(coeff);
	for (vector<vector<float> >::iterator it = fir.begin(); it != fir.end(); ++it)
		vector<float>().swap(*it);
}

bool LowPass::process(const vector<sample_t> &inBuffer, vector<sample_t> &outBuffer)
{
	const float *in = (const float*)inBuffer.data();
	float *out = (float*)outBuffer.data();
	unsigned int channels = inputChannels(); /* = outputChannels */
	unsigned int insize = inBuffer.size();
	unsigned int inframes = insize / channels;

	while (inframes--) {
		/* FIXME: Could store last _firLength-1 frames from previous
		 * block and then read samples in-situ.  This would eliminate the
		 * need for managing this ring buffer.
		 */
		for (unsigned int c = 0; c < channels; c++)
			fir[c][headpos] = *in++;

		if (++decimationCount == _decimation) {
			for (unsigned int c = 0; c < channels; c++)
				out[c] = 0.0;

			/* Generate output */
			unsigned int idx = headpos;
			for (unsigned int tap = 0; tap < _firLength; tap++) {
				float weight = coeff[tap];
				for (unsigned int c = 0; c < channels; c++) {
					out[c] += weight * fir[c][idx];
				}
				idx = (idx - 1) & (_firLength - 1);
			}
			out += channels;
			decimationCount = 0;
		}

		headpos = (headpos + 1) & (_firLength - 1);
	}

	return true;
}

void LowPass::recalculate()
{
	/* Determine cutoff bin for desired pass band */
	unsigned int maxbin = _firLength * _passband / inputSampleRate() / 2;

	/* Filter spec is pure-real so -ve frequency components are the
	 * same as +ve (conjugate, but no imaginary part).
	 */
	unsigned int mask = _firLength - 1;
	for (unsigned int n = 0; n < _firLength / 2 + 1; n++) {
		spec[n][0] = spec[(_firLength - n) & mask][0] =
				(n < maxbin) ? 1.0 : 0.0;
		spec[n][1] = spec[(_firLength - n) & mask][1] =
				0.0;
	}

	fftwf_execute(p);

	coeff.resize(_firLength);
	for (unsigned int n = 0; n < _firLength; n++) {
		unsigned int bin = (n + _firLength / 2) & (_firLength - 1);

		/* Response is real, so discard imaginary component,
		 * re-order into FIR coefficient vector, apply window */
		coeff[n] = impulse[bin][0] * window[n];
	}

	/* Dump impulse response */
	for (vector<float>::iterator it = coeff.begin(); it != coeff.end(); ++it)
		LOG_DEBUG("%f\n", *it);
}
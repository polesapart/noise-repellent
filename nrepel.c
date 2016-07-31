/*
noise-repellent -- Noise Reduction LV2

Copyright 2016 Luciano Dato <lucianodato@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/
*/


#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <fftw3.h>


#include "denoise_gain.c"
#include "estimate_noise_spectrum.c"
#include "extra_functions.c"


#include "lv2/lv2plug.in/ns/lv2core/lv2.h"

#define NREPEL_URI "https://github.com/lucianodato/noise-repellent"

//Noise capture states
#define MANUAL_CAPTURE_OFF_STATE 0
#define MANUAL_CAPTURE_ON_STATE 1
#define AUTO_CAPTURE_STATE 2

//STFT default values
#define DEFAULT_FFT_SIZE 2048 //This should be an even number (Cooley-Turkey)
#define DEFAULT_WINDOW_TYPE 0 //0 Hann 1 Hamming 2 Blackman
#define DEFAULT_OVERLAP_FACTOR 4 //2 is 50% and 4 is 75% overlap

#define USE_MAGNITUDE false //To use magnitude or power spectrum for computing
#define NOISE_MEAN_CHOISE 0 //0 max 1 geometric mean 2 average
#define DENOISE_METHOD 0 //0 Wiener 1 Power Substraction

///---------------------------------------------------------------------

//LV2 CODE

typedef enum {
	NREPEL_INPUT  = 0,
	NREPEL_OUTPUT = 1,
	NREPEL_CAPTURE = 2,
	NREPEL_AMOUNT = 3,
	NREPEL_LATENCY = 4,
} PortIndex;

typedef struct {
	const float* input; //input of samples from host (changing size)
	float* output; //output of samples to host (changing size)
	float samp_rate; // Sample rate received from the host

	//Parameters for the algorithm (user input)
	int* capt_state; // Capture Noise state (Manual-Off-Auto)
	int* fft_option; //selected fft size by the user
	float* amount_reduc; // Amount of noise to reduce in dB
	int* report_latency; // Latency necessary
	int noise_mean_choise;
	int denoise_method;
	bool use_mag;
	float max;

	//Parameters for the STFT
	int fft_size; //FFTW input size
	int fft_size_2; //FFTW half input size
	int window_type; // Window type for the STFT
	int overlap_factor; //oversampling factor for overlap calculations
	int hop; // Hop size for the STFT
	float* window; // Window values

	//Temporary buffers for processing and outputting
	int input_latency;
	float* in_fifo; //internal input buffer
	float* out_fifo; //internal output buffer
	float* output_accum; //FFT output accumulator
	int read_ptr; //buffers read pointer

	//FFTW related variables
	float* input_fft_buffer;
	float* output_fft_buffer;
	fftwf_plan forward;
	fftwf_plan backward;

	float real_p,real_n,mag,p2;
	float* fft_magnitude;//magnitude
	float* fft_p2;//power

	//Store variables
	float* noise_print_min; // The min noise spectrum computed by the captured signal
	float* noise_print_max; // The max noise spectrum computed by the captured signal
	float* noise_print_avg; // The avg noise spectrum computed by the captured signal
	float* noise_spectrum;
	float* Gk;

} Nrepel;

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
						double                    rate,
						const char*               bundle_path,
						const LV2_Feature* const* features) {
	//Actual struct declaration
	Nrepel* nrepel = (Nrepel*)malloc(sizeof(Nrepel));

	//Initialize variables
	nrepel->samp_rate = rate;
	nrepel->fft_size = DEFAULT_FFT_SIZE;
	nrepel->window_type = DEFAULT_WINDOW_TYPE;
	nrepel->overlap_factor = DEFAULT_OVERLAP_FACTOR;
	nrepel->noise_mean_choise = NOISE_MEAN_CHOISE;
	nrepel->denoise_method = DENOISE_METHOD;
	nrepel->use_mag = USE_MAGNITUDE;
	nrepel->max = FLT_MAX;

	nrepel->fft_size_2 = nrepel->fft_size/2;
	nrepel->hop = nrepel->fft_size/nrepel->overlap_factor;
	nrepel->input_latency = nrepel->fft_size - nrepel->hop;
	nrepel->read_ptr = nrepel->input_latency; //the initial position because we are that many samples ahead

	nrepel->in_fifo = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->out_fifo = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->output_accum = (float*)malloc(sizeof(float)*nrepel->fft_size);

	nrepel->window = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->input_fft_buffer = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->output_fft_buffer = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->forward = fftwf_plan_r2r_1d(nrepel->fft_size, nrepel->input_fft_buffer, nrepel->output_fft_buffer, FFTW_R2HC, FFTW_ESTIMATE);
	nrepel->backward = fftwf_plan_r2r_1d(nrepel->fft_size, nrepel->output_fft_buffer, nrepel->input_fft_buffer, FFTW_HC2R, FFTW_ESTIMATE);

	nrepel->fft_magnitude = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->fft_p2 = (float*)malloc(sizeof(float)*nrepel->fft_size);

	nrepel->noise_print_min = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->noise_print_max = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->noise_print_avg = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->noise_spectrum = (float*)malloc(sizeof(float)*nrepel->fft_size);
	nrepel->Gk = (float*)malloc(sizeof(float)*nrepel->fft_size);

	//Here we initialize arrays with zeros
	memset(nrepel->in_fifo, 0, nrepel->fft_size*sizeof(float));
	memset(nrepel->out_fifo, 0, nrepel->fft_size*sizeof(float));
	memset(nrepel->input_fft_buffer, 0, nrepel->fft_size*sizeof(float));
	memset(nrepel->output_fft_buffer, 0, nrepel->fft_size*sizeof(float));
	memset(nrepel->output_accum, 0, nrepel->fft_size*sizeof(float));
	memset(nrepel->fft_magnitude, 0, nrepel->fft_size*sizeof(float));
	memset(nrepel->fft_p2, 0, nrepel->fft_size*sizeof(float));

	memset(nrepel->noise_print_min, nrepel->max, nrepel->fft_size*sizeof(float));
	memset(nrepel->noise_print_max, 0, nrepel->fft_size*sizeof(float));
	memset(nrepel->noise_print_min, 0, nrepel->fft_size*sizeof(float));
	memset(nrepel->noise_spectrum, 0, nrepel->fft_size*sizeof(float));
	memset(nrepel->Gk, 1, nrepel->fft_size*sizeof(float));

	fft_window(nrepel->window,nrepel->fft_size,nrepel->window_type); //Init window

	return (LV2_Handle)nrepel;
}



static void
connect_port(LV2_Handle instance,
						 uint32_t   port,
						 void*      data) {
	Nrepel* nrepel = (Nrepel*)instance;

	switch ((PortIndex)port) {
		case NREPEL_INPUT:
		nrepel->input = (const float*)data;
		break;
		case NREPEL_OUTPUT:
		nrepel->output = (float*)data;
		break;
		case NREPEL_CAPTURE:
		nrepel->capt_state = (int*)data;
		break;
		case NREPEL_AMOUNT:
		nrepel->amount_reduc = (float*)data;
		break;
		case NREPEL_LATENCY:
		nrepel->report_latency = (int*)data;
		break;
	}
}

static void
activate(LV2_Handle instance)
{
}

static void
run(LV2_Handle instance, uint32_t n_samples) {
	Nrepel* nrepel = (Nrepel*)instance;

	//handy variables
	int k;
	unsigned int pos;
	float nor_coef;

	//Inform latency at run call
	*(nrepel->report_latency) = nrepel->input_latency;

	//main loop for processing
	for (pos = 0; pos < n_samples; pos++){
		//Store samples int the input buffer
		nrepel->in_fifo[nrepel->read_ptr] = nrepel->input[pos];
		//Output samples in the output buffer (even zeros introduced by latency)
		nrepel->output[pos] = nrepel->out_fifo[nrepel->read_ptr - nrepel->input_latency];
		//Now move the read pointer
		nrepel->read_ptr++;

		//Once the buffer is full we can do stuff
		nor_coef = 0.f;
		if (nrepel->read_ptr >= nrepel->fft_size){
			//Reset the input buffer position
			nrepel->read_ptr = nrepel->input_latency;

			//If capturing noise normalize input
			if (true) { //*(nrepel->capt_state) == MANUAL_CAPTURE_ON_STATE
				for (k = 0; k < nrepel->fft_size; k++){
					nor_coef += MAX(nor_coef,fabs(nrepel->in_fifo[k]));
				}

				for (k = 0; k < nrepel->fft_size; k++){
					nrepel->in_fifo[k] = sanitize_denormal(nrepel->in_fifo[k]/nor_coef);
				}
			}

			//Apply windowing (Could be any window type)
			for (k = 0; k < nrepel->fft_size; k++){
				nrepel->input_fft_buffer[k] = sanitize_denormal(nrepel->in_fifo[k] * nrepel->window[k]);
			}

			//----------FFT Analysis------------
			//Do transform
			fftwf_execute(nrepel->forward);

			//Get the positive spectrum and compute magnitude and phase response
			//k=0 is DC freq so it dont get considered
			for (k = 0; k <= nrepel->fft_size_2; k++){
				nrepel->real_p = nrepel->output_fft_buffer[k];
				nrepel->real_n = nrepel->output_fft_buffer[nrepel->fft_size-k];

				//Get mag and power
				if(k < nrepel->fft_size){
					nrepel->p2 = nrepel->real_p*nrepel->real_p + nrepel->real_n*nrepel->real_n;
					nrepel->mag = sqrtf(nrepel->p2);
				} else {
					//Nyquist
					nrepel->p2 = nrepel->real_p*nrepel->real_p;
					nrepel->mag = nrepel->real_p;
				}

				//Store values in magnitude and phase arrays
				nrepel->fft_magnitude[k] = sanitize_denormal(nrepel->mag);
				nrepel->fft_p2[k] = sanitize_denormal(nrepel->p2);
			}

			//------------Processing---------------

			//If selected estimate noise spectrum
			switch (*(nrepel->capt_state)) {
				case MANUAL_CAPTURE_ON_STATE:
					estimate_noise_spectrum(nrepel->noise_mean_choise,nrepel->use_mag,nrepel->fft_magnitude,nrepel->fft_p2,*(nrepel->capt_state),nrepel->noise_print_min,nrepel->noise_print_max,nrepel->noise_print_avg,nrepel->fft_size_2,nrepel->fft_size,nrepel->noise_spectrum);
					break;
				case MANUAL_CAPTURE_OFF_STATE:
					//Apply denoising based on previously computed spectrum (manual or automatic)
					denoise_gain(nrepel->denoise_method,nrepel->use_mag,from_dB(*(nrepel->amount_reduc)),nrepel->fft_magnitude,nrepel->fft_p2,nrepel->fft_size_2,nrepel->Gk,nrepel->noise_spectrum);

					//Apply the calculated gain to the signal
					for (k = 0; k <= nrepel->fft_size_2; k++) {
						nrepel->output_fft_buffer[k] *=nrepel->Gk[k];
						if(k < nrepel->fft_size_2)
							nrepel->output_fft_buffer[nrepel->fft_size-k] *= nrepel->Gk[k];
					}
					break;
				case AUTO_CAPTURE_STATE:
					//if slected auto estimate noise spectrum and apply denoising
					estimate_noise_spectrum(nrepel->noise_mean_choise,nrepel->use_mag,nrepel->fft_magnitude,nrepel->fft_p2,*(nrepel->capt_state),nrepel->noise_print_min,nrepel->noise_print_max,nrepel->noise_print_avg,nrepel->fft_size_2,nrepel->fft_size,nrepel->noise_spectrum);

					//Apply denoising based on previously computed spectrum (manual or automatic)
					denoise_gain(nrepel->denoise_method,nrepel->use_mag,from_dB(*(nrepel->amount_reduc)),nrepel->fft_magnitude,nrepel->fft_p2,nrepel->fft_size_2,nrepel->Gk,nrepel->noise_spectrum);

					//Apply the calculated gain to the signal
					for (k = 0; k <= nrepel->fft_size_2; k++) {
						nrepel->output_fft_buffer[k] *=nrepel->Gk[k];
						if(k < nrepel->fft_size_2)
							nrepel->output_fft_buffer[nrepel->fft_size-k] *= nrepel->Gk[k];
					}
					break;
			}

			//------------FFT Synthesis-------------

			//Do inverse transform
			fftwf_execute(nrepel->backward);

			//Scaling FFT (because is unnormalized)
			for(k = 0; k < nrepel->fft_size; k++){
				nrepel->input_fft_buffer[k] = sanitize_denormal(nrepel->input_fft_buffer[k]/nrepel->fft_size);
			}

			// //unnormalize if was capturing noise
			// if (*(nrepel->capt_state) == MANUAL_CAPTURE_ON_STATE) {
			// 	for(k = 0; k < nrepel->fft_size; k++){
			// 		nrepel->input_fft_buffer[k] = sanitize_denormal(nrepel->input_fft_buffer[k]*nor_coef);
			// 	}
			// }

			//Accumulate (Overlapadd)
			for(k = 0; k < nrepel->fft_size; k++){
				nrepel->output_accum[k] += nrepel->input_fft_buffer[k]*nrepel->hop;
			}

			//Output samples up to the hop size
			for (k = 0; k < nrepel->hop; k++){
				nrepel->out_fifo[k] = nrepel->output_accum[k];
			}

			//shift FFT accumulator the hop size
			memmove(nrepel->output_accum, nrepel->output_accum + nrepel->hop, nrepel->fft_size*sizeof(float));

			//Make sure the non overlaping section to be 0
			for (k = (nrepel->fft_size-nrepel->hop); k < nrepel->fft_size; k++){
				nrepel->output_accum[k] = 0.f;
			}

			//move input FIFO
			for (k = 0; k < nrepel->input_latency; k++){
				nrepel->in_fifo[k] = nrepel->in_fifo[k+nrepel->hop];
			}
		}//if
	}//main loop
}

static void
deactivate(LV2_Handle instance)
{
}

static void
cleanup(LV2_Handle instance)
{
	free(instance);
}

const void*
extension_data(const char* uri)
{
	return NULL;
}

static const
LV2_Descriptor descriptor = {
	NREPEL_URI,
	instantiate,
	connect_port,
	activate,
	run,
	deactivate,
	cleanup,
	extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
	switch (index) {
		case 0:
		return &descriptor;
		default:
		return NULL;
	}
}

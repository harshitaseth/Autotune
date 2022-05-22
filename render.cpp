/*
 ____  _____ _        _    
| __ )| ____| |      / \   
|  _ \|  _| | |     / _ \  
| |_) | |___| |___ / ___ \ 
|____/|_____|_____/_/   \_\

http://bela.io

C++ Real-Time Audio Programming with Bela - Lecture 20: Phase vocoder, part 3
fft-pitchshift: change the pitch of a sound without changing its timing using FFTs
*/

#include <Bela.h>
#include <libraries/Fft/Fft.h>
#include <libraries/Scope/Scope.h>
#include <libraries/Gui/Gui.h>
#include <libraries/GuiController/GuiController.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include "MonoFilePlayer.h"
#include "targetfreq.h"
#include <libraries/Midi/Midi.h>

Midi gMidi;
const char* gMidiPort0 = "hw:1,0,0";
// FFT-related variables
Fft gFft;							// FFT processing object
const int gFftSize = 1024;			// FFT window size in samples
const int gHopSize = 128;			// How often we calculate a window
float gScaleFactor = 0.5;			// How much to scale the output, based on window type and overlap
float gPitchShift = 1.0;			// Ratio of output to input frequency
int counter = 0;
// Circular buffer and pointer for assembling a window of samples
const int gBufferSize = 16384;
std::vector<float> gInputBuffer;
std::vector<float> gInputBuffer_fundamental;
int gInputBufferPointer = 0;
int gHopCounter = 0;
float gPhase = 0;
float gFrequency = 0;
float gAmplitude = 0;
double sampleRate;
float gLastPitchShift = 0.0;
float gpartialshift = 0.0;
float gsmoothingfactor = 0.0;

// Circular buffer for collecting the output of the overlap-add process
std::vector<float> gOutputBuffer;
float fundamental_freq = 0.0;
// Start the write pointer ahead of the read pointer by at least window + hop, with some margin
int gOutputBufferWritePointer = gFftSize + 2*gHopSize;
int gOutputBufferReadPointer = 0;

// Buffer to hold the windows for FFT analysis and synthesis
std::vector<float> gAnalysisWindowBuffer;
std::vector<float> gSynthesisWindowBuffer;

// Name of the sound file (in project folder)
std::string gFilename = "file_1.wav"; 

// Object that handles playing sound from a buffer
MonoFilePlayer gPlayer;

// Thread for FFT processing
AuxiliaryTask gFftTask;
int gCachedInputBufferPointer = 0;

void process_fft_background(void *);

// Bela oscilloscope
Scope gScope;

// Browser-based GUI to adjust parameters
Gui gGui;
GuiController gGuiController;

bool setup(BelaContext *context, void *userData)
{
	// Load the audio file
	if(!gPlayer.setup(gFilename)) {
    	rt_printf("Error loading audio file '%s'\n", gFilename.c_str());
    	return false;
	}

	// Print some useful info
    rt_printf("Loaded the audio file '%s' with %d frames (%.1f seconds)\n", 
    			gFilename.c_str(), gPlayer.size(),
    			gPlayer.size() / context->audioSampleRate);
	
	// Set up the FFT and its buffers
	gFft.setup(gFftSize);
	gInputBuffer.resize(gBufferSize);
	gOutputBuffer.resize(gBufferSize);
	gInputBuffer_fundamental.resize(context->audioFrames);
	
	// Calculate the windows
	gAnalysisWindowBuffer.resize(gFftSize);
	gSynthesisWindowBuffer.resize(gFftSize);
	for(int n = 0; n < gFftSize; n++) {
		// Hann window, split across analysis and synthesis windows
		gAnalysisWindowBuffer[n] = 0.5f * (1.0f - cosf(2.0 * M_PI * n / (float)(gFftSize - 1)));
		gSynthesisWindowBuffer[n] = gAnalysisWindowBuffer[n];
	}
	
	// Initialise the oscilloscope
	gScope.setup(2, context->audioSampleRate);
	
	// Set up the GUI
	gGui.setup(context->projectName);
	gGuiController.setup(&gGui, "Pitch Shift Variation");	
	
	// Arguments: name, minimum, maximum, increment, default value
	gGuiController.addSlider("Partial Pitch Shift", 0, 0, 1, 0);
	gGuiController.addSlider("Smooth Pitch Shift", 0, 0, 0.05, 0);
	
	
	// Set up the thread for the FFT
	gFftTask = Bela_createAuxiliaryTask(process_fft_background, 50, "bela-process-fft");
	
	sampleRate = context->audioSampleRate;


	
	return true;
}



// Wrap the phase to the range -pi to pi
float wrapPhase(float phaseIn)
{
    if (phaseIn >= 0)
        return fmodf(phaseIn + M_PI, 2.0 * M_PI) - M_PI;
    else
        return fmodf(phaseIn - M_PI, -2.0 * M_PI) + M_PI;	
}






// This function handles the FFT processing in this example once the buffer has
// been assembled.

void process_fft(std::vector<float> const& inBuffer, unsigned int inPointer, std::vector<float>& outBuffer, unsigned int outPointer)
{
	static std::vector<float> unwrappedBuffer(gFftSize);	// Container to hold the unwrapped time-domain values
	
	static std::vector<float> lastInputPhases(gFftSize);	// Hold the phases from the previous hop of input signal
	static std::vector<float> lastOutputPhases(gFftSize);	// and output (synthesised) signal
	
	// These containers hold the converted representation from magnitude-phase
	// into magnitude-frequency, used for pitch shifting
	static std::vector<float> analysisMagnitudes(gFftSize / 2 + 1);
	static std::vector<float> analysisFrequencies(gFftSize / 2 + 1);
	static std::vector<float> synthesisMagnitudes(gFftSize / 2 + 1);
	static std::vector<float> synthesisFrequencies(gFftSize / 2 + 1);
	static std::vector<float> synthesfundamental(gFftSize / 2 + 1);
	static std::vector<float> synthesisautotune(gFftSize / 2 + 1);
	static std::vector<float> V(gFftSize / 2 + 1);
	
	// Copy buffer into FFT input
	for(int n = 0; n < gFftSize; n++) {
		// Use modulo arithmetic to calculate the circular buffer index
		int circularBufferIndex = (inPointer + n - gFftSize + gBufferSize) % gBufferSize;
		unwrappedBuffer[n] = inBuffer[circularBufferIndex] * gAnalysisWindowBuffer[n];
	}
	
	// Process the FFT based on the time domain input
	gFft.fft(unwrappedBuffer);
		
	// Analyse the lower half of the spectrum. The upper half is just
	// the complex conjugate and does not contain any unique information
	for(int n = 0; n <= gFftSize/2; n++) {
		// Turn real and imaginary components into amplitude and phase
		float amplitude = gFft.fda(n);
		float phase = atan2f(gFft.fdi(n), gFft.fdr(n));
		
		// Calculate the phase difference in this bin between the last
		// hop and this one, which will indirectly give us the exact frequency
		float phaseDiff = phase - lastInputPhases[n];
		
		// Subtract the amount of phase increment we'd expect to see based
		// on the centre frequency of this bin (2*pi*n/gFftSize) for this
		// hop size, then wrap to the range -pi to pi
		float binCentreFrequency = 2.0 * M_PI * (float)n / (float)gFftSize;
		phaseDiff = wrapPhase(phaseDiff - binCentreFrequency * gHopSize);
		
		// Find deviation in (fractional) number of bins from the centre frequency
		float binDeviation = phaseDiff * (float)gFftSize / (float)gHopSize / (2.0 * M_PI);
		
		// Add the original bin number to get the fractional bin where this partial belongs
		analysisFrequencies[n] = (float)n + binDeviation;
		
		// Save the magnitude for later and for the GUI
		analysisMagnitudes[n] = amplitude;
		
		// Save the phase for next hop
		lastInputPhases[n] = phase;
	}	

	// Zero out the synthesis bins, ready for new data
	for(int n = 0; n <= gFftSize/2; n++) {
		synthesisMagnitudes[n] = synthesisFrequencies[n] = 0;
	}
	
	// calculating fundamental frequency
	//variable to store three peaks with indices
	float max_1 = 0;
	int max_index_1 = 0;
	float max_2 = 0;
	int max_index_2 = 0;
	float max_3 = 0;
	int max_index_3 = 0;	
	for(int n = 0; n < gFftSize/2; n++) {
		float amplitude = analysisMagnitudes[n];
		float mag = amplitude;
		if (mag > max_3 && mag < max_2 && mag < max_1){
			max_3 = mag;
			max_index_3 = n;
		}
		else if(mag > max_3 && mag > max_2 && mag < max_1){
			max_2 = mag;
			max_index_2 = n;
		}
		else if(mag > max_3 && mag > max_2 && mag > max_1){
			max_1 = mag;
			max_index_1 = n;
		}
		
	}
	
	int max_bin_index = 0;
	// Getting indices with minimum bin out of 3 maximum amplitude frequency
	if(max_index_1 < max_index_2 && max_index_1 < max_index_3){
		max_bin_index = max_index_1;
	}
		if(max_index_2 < max_index_1 && max_index_2 < max_index_3){
		max_bin_index = max_index_2;
	}
		if(max_index_3 < max_index_1 && max_index_3 < max_index_2){
		max_bin_index = max_index_3;
	}


	fundamental_freq = analysisFrequencies[max_bin_index];
	double bin_number = analysisFrequencies[max_bin_index];
	fundamental_freq = bin_number*sampleRate/(gFftSize);

	double target= getTargetFreq(fundamental_freq,counter);
	gPitchShift = target/fundamental_freq;
	
	// calculating pitch shift ratio with partial shift frequency

	if (gpartialshift > 0.5){
		double partialtarget =  0.0;
		// mapping close to the target scale frequency
		if ((target - fundamental_freq) > 0)
  			partialtarget = fundamental_freq + (target - fundamental_freq)/2;
		 else
  			partialtarget = fundamental_freq - (fundamental_freq - target)/2;
		gPitchShift = partialtarget/fundamental_freq;
		}
	
	// adding condition for smooth pitch shift
	if (gsmoothingfactor >0){
		if (gPitchShift - gLastPitchShift > gsmoothingfactor)
			gPitchShift = gLastPitchShift + gsmoothingfactor;
		else if(gLastPitchShift - gPitchShift > gsmoothingfactor)
			gPitchShift = gLastPitchShift - gsmoothingfactor;
	}

	// copying the last pitch shift ratio
	gLastPitchShift = gPitchShift;
	

	
	// Handle the pitch shift, storing frequencies into new bins
	for(int n = 0; n <= gFftSize/2; n++) {
		// Find the nearest bin to the shifted frequency
		
		int newBin = floorf(n * gPitchShift + 0.5);	
		
		// Ignore any bins that have shifted above Nyquist
		if(newBin <= gFftSize / 2) {
			synthesisMagnitudes[newBin] += analysisMagnitudes[n];
			synthesisFrequencies[newBin] = analysisFrequencies[n] * gPitchShift;
		
		}
	}
	
	
	
		
	// Synthesise frequencies into new magnitude and phase values for FFT bins
	for(int n = 0; n <= gFftSize / 2; n++) {
		float amplitude = synthesisMagnitudes[n];
		
		// Get the fractional offset from the bin centre frequency
		float binDeviation = synthesisFrequencies[n] - n;
		
		// Multiply to get back to a phase value
		float phaseDiff = binDeviation * 2.0 * M_PI * (float)gHopSize / (float)gFftSize;
		
		// Add the expected phase increment based on the bin centre frequency
		float binCentreFrequency = 2.0 * M_PI * (float)n / (float)gFftSize;
		phaseDiff +=  binCentreFrequency * gHopSize;
		
		// Advance the phase from the previous hop
		float outPhase = wrapPhase(lastOutputPhases[n] + phaseDiff);
		
		// Now convert magnitude and phase back to real and imaginary components
		gFft.fdr(n) = amplitude * cosf(outPhase);
		gFft.fdi(n) = amplitude * sinf(outPhase);
		
		// Also store the complex conjugate in the upper half of the spectrum
		if(n > 0 && n < gFftSize / 2) {
			gFft.fdr(gFftSize - n) = gFft.fdr(n);
			gFft.fdi(gFftSize - n) = -gFft.fdi(n);
		}
		
		// Save the phase for the next hop
		lastOutputPhases[n] = outPhase;
	}
		
	// Run the inverse FFT
	gFft.ifft();
	
	// Add timeDomainOut into the output buffer
	for(int n = 0; n < gFftSize; n++) {
		int circularBufferIndex = (outPointer + n - gFftSize + gBufferSize) % gBufferSize;
		outBuffer[circularBufferIndex] += gFft.td(n) * gSynthesisWindowBuffer[n];
	}
}

// This function runs in an auxiliary task on Bela, calling process_fft
void process_fft_background(void *)
{
	process_fft(gInputBuffer, gCachedInputBufferPointer, gOutputBuffer, gOutputBufferWritePointer);

	// Update the output buffer write pointer to start at the next hop
	gOutputBufferWritePointer = (gOutputBufferWritePointer + gHopSize) % gBufferSize;
}







void render(BelaContext *context, void *userData)
{
	// Get the partialshift and smoothingfactor from the GUI slider and convert to ratio
	gpartialshift = gGuiController.getSliderValue(0);	
	gsmoothingfactor = gGuiController.getSliderValue(1);

	

	
	for(unsigned int n = 0; n < context->audioFrames; n++) {
        // Read the next sample from the buffer
        float in = gPlayer.process();
        
        

		// Store the sample ("in") in a buffer for the FFT
		// Increment the pointer and when full window has been 
		// assembled, call process_fft()
		gInputBuffer[gInputBufferPointer++] = in;
		if(gInputBufferPointer >= gBufferSize) {
			// Wrap the circular buffer
			// Notice: this is not the condition for starting a new FFT
			gInputBufferPointer = 0;
		}
		
		// Get the output sample from the output buffer
		float out = gOutputBuffer[gOutputBufferReadPointer];
		
		// Then clear the output sample in the buffer so it is ready for the next overlap-add
		gOutputBuffer[gOutputBufferReadPointer] = 0;
		
		// Scale the output down by the scale factor, compensating for the overlap
		out *= gScaleFactor;
		
		// Increment the read pointer in the output cicular buffer
		gOutputBufferReadPointer++;
		if(gOutputBufferReadPointer >= gBufferSize)
			gOutputBufferReadPointer = 0;
		
		// Increment the hop counter and start a new FFT if we've reached the hop size
		if(++gHopCounter >= gHopSize) {
			gHopCounter = 0;
			
			gCachedInputBufferPointer = gInputBufferPointer;
			Bela_scheduleAuxiliaryTask(gFftTask);
		}

		// Write the audio to the output
		for(unsigned int channel = 0; channel < context->audioOutChannels; channel++) {
			audioWrite(context, n, channel, out*5);
		}
	
		// Log to the Scope
		gScope.log(in, out);
	}
}

void cleanup(BelaContext *context, void *userData)
{

}

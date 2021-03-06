/*
 *   This file is part of OpenTLD.
 *
 *   OpenTLD is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   OpenTLD is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with OpenTLD.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "DetectorCascade.h"

#include <cstdlib>
#include <cmath>
#include <assert.h>

#include <opencv/cv.h>

#include "EnsembleClassifier.h"

using namespace std;
using namespace cv;

namespace tld
{

static int sub2idx(float x,  float y, int widthstep) {
	return (int)(floor((x)+0.5) + floor((y)+0.5)*widthstep);
}

EnsembleClassifier::EnsembleClassifier() :
    features(NULL),
    featureOffsets(NULL),
    posteriors(NULL),
    positives(NULL),
    negatives(NULL)
{
	//numTrees = nt;
	//numFeatures = nf;
	enabled = true;

	thetaFP_learn = 0.6;
	thetaTP_learn = 0.4;
}

EnsembleClassifier::~EnsembleClassifier()
{
    release();
}


void EnsembleClassifier::init() {
	numTrees = nt;
	numFeatures =  nf;
	numIndices = pow(2.0f, numFeatures);

	initFeatureLocations();
	initFeatureOffsets();
	initPosteriors();
}

void EnsembleClassifier::release()
{
    delete[] features;
    features = NULL;
    delete[] featureOffsets;
    featureOffsets = NULL;
    delete[] posteriors;
    posteriors = NULL;
    delete[] positives;
    positives = NULL;
    delete[] negatives;
    negatives = NULL;
}

/*
 * Generates random measurements in the format <x1,y1,x2,y2>
 */
void EnsembleClassifier::initFeatureLocations() {
	//assert (numTrees == nt);
	//assert (numFeatures == nf);
	
	int size = 2 * 2 * numFeatures * numTrees;
	
	features = new float[size];
	for(int i = 0; i < size; i++) {
		features[i] = rand() / (float)RAND_MAX;
	}
}

void EnsembleClassifier::initFeatureOffsets() {

	featureOffsets = new int[numScales * numTrees * numFeatures * 2];
	int *off = featureOffsets;
	
	assert (numTrees == nt);
	assert (numFeatures == nf);

	for(int k = 0; k < numScales; k++) {
		Size scale = scales[k];

		for(int i = 0; i < numTrees; i++) {
			for(int j = 0; j < numFeatures; j++) {
				float *currentFeature  = features + (4 * numFeatures) * i + 4 * j;
				*off++ = sub2idx((scale.width - 1) * currentFeature[0] + 1, (scale.height - 1) * currentFeature[1] + 1, imgWidthStep); //We add +1 because the index of the bounding box points to x-1, y-1
				*off++ = sub2idx((scale.width - 1) * currentFeature[2] + 1, (scale.height - 1) * currentFeature[3] + 1, imgWidthStep);
			}
		}
	}
}

void EnsembleClassifier::initPosteriors() {
	//assert (numTrees == nt);
	//assert (numFeatures == nf);
	posteriors = new float[numTrees * numIndices];
	positives = new int[numTrees * numIndices];
	negatives = new int[numTrees * numIndices];

	for(int i = 0; i < numTrees; i++) {
		for(int j = 0; j < numIndices; j++) {
			posteriors[i * numIndices + j] = 0;
			positives[i * numIndices + j] = 0;
			negatives[i * numIndices + j] = 0;
		}
	}
}

void EnsembleClassifier::nextIteration(const Mat &img)
{
    if(!enabled) return;

    this->img = (const unsigned char *)img.data;
}

//Classical fern algorithm
int EnsembleClassifier::calcFernFeature(int windowIdx, int treeIdx)
{

    int index = 0;
    int *bbox = windowOffsets + windowIdx * TLD_WINDOW_OFFSET_SIZE;
    int *off = featureOffsets + bbox[4] + treeIdx * 2 * numFeatures; //bbox[4] is pointer to features for the current scale

    for(int i = 0; i < numFeatures; i++)
    {
        index <<= 1;

	// pixel comparison
        int fp0 = img[bbox[0] + off[0]];
        int fp1 = img[bbox[0] + off[1]];

        if(fp0 > fp1)
        {
            index |= 1;
        }

        off += 2;
    }

    return index;
}

void EnsembleClassifier::calcFeatureVector(int windowIdx, int *featureVector)
{
	//cout << "......calcFernFeature...\n";
	//assert (numTrees == nt);
	//assert (numFeatures == nf);
	for(int i = 0; i < numTrees; i++) {
		featureVector[i] = calcFernFeature(windowIdx, i);
	}
}

void EnsembleClassifier::classifyWindow(int windowIdx) {
	//assert (numTrees == nt);
	//assert (numFeatures == nf);
	int *featureVector = detectionResult->featureVectors + numTrees * windowIdx;
    	//cout << "...calcFeatureVector...\n";
	calcFeatureVector(windowIdx, featureVector);
	
	//cout << "...calcConfidence...\n";
	float conf = 0.0;

	for(int i = 0; i < numTrees; i++) {
		//cout << "......adding tree " << i << "'s result, ";
		conf += posteriors[i * numIndices + featureVector[i]];
		//cout << "conf = " << conf << "...\n";
	}
	
	detectionResult->posteriors[windowIdx] = conf;

	return;
}

bool EnsembleClassifier::filter(int i) {
	if(!enabled) return true;

	classifyWindow(i);
	cout << "...Ensemble Classifier done!\n";

	if(detectionResult->posteriors[i] < 0.5) return false;
	return true;
}

void EnsembleClassifier::learn(int *boundary, int positive, int *featureVector) {
	if(!enabled) return;
	
	float conf = 0.0;
	for(int i = 0; i < numTrees; i++) {
		conf += posteriors[i * numIndices + featureVector[i]];
	}

	if((positive && conf < thetaTP_learn) || (!positive && conf > thetaFP_learn)) {
		// update posteriors
		for ( int i = 0; i < numTrees; i++ ) {
			int idx = i * numIndices + featureVector[i];
			(positive) ? positives[idx] += 1 : negatives[idx] += 1;
			posteriors[idx] = ((float) positives[idx]) / (positives[idx] + negatives[idx]);
		}
	}
}


} /* namespace tld */

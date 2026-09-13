#pragma once

#include <iostream>
#include <string>
#include <iomanip>

#include <Windows.h>

#include <sensorsapi.h>
#include <sensors.h>
#pragma comment(lib, "sensorsapi.lib")

#include "AccelerometerReader.h"


class HingeSensorReader
{
private:
    ISensorManager* pSensorManager = NULL;
    ISensorCollection* pSensorCollection = NULL;
    ISensor* pSensor = NULL;
    IPortableDeviceKeyCollection* pDataFields = NULL;

    PROPERTYKEY pAngleKey, pLidAngleKey, pBodyAngleKey;

    AccelerometerReader accelerometerReader;

    void ThrowIfFailed(HRESULT hr) {
        if (FAILED(hr)) throw hr;
    }

public:
    HingeSensorReader();
    ~HingeSensorReader();

    HRESULT Init();

    bool IsReady() const;

    HRESULT GetHingeAngle(int* angle, int* lidAngle, int* bodyAngle);
    HRESULT GetHingeAngleFloat(float* angle);

    bool useRawAccelerometer = false;
};
#ifndef DRIVER_NAME_H
#define DRIVER_NAME_H

#define MONITOR_NAME			"wiz3D Monitor"
#define MONITOR_INF_FILE		"wiz3dmonitor.inf"
#define FULL_PRODUCT_NAME		"wiz3D"
#define PRODUCT_NAME			"wiz3D"
#define COMPANY_NAME			"wiz3D"
#define SHORT_COMPANY_NAME		"wiz3D"
#define CONTROL_CENTER			"wiz3D Control Center"
#define COMPANY_SITE			"https://github.com/Stereopticon-Org/wiz3D"
#define SETUP_FILE_NAME			"wiz3DSetup"

//------------------------ section for C++ code only --------------
#ifdef	_MSC_VER
#define  S3DWRAPPERD3D7_DLL_NAME		TEXT("S3DWrapperD3D7.dll")
#define  S3DWRAPPERD3D8_DLL_NAME		TEXT("S3DWrapperD3D8.dll")
#define  S3DWRAPPERD3D9_DLL_NAME		TEXT("S3DWrapperD3D9.dll")
#define  S3DWRAPPERD3D10_DLL_NAME		TEXT("S3DWrapperD3D10.dll")
#define  S3DWRAPPEROGL_DLL_NAME			TEXT("S3DWrapperOGL.dll")
#define  S3DINJECTOR_DLL_NAME			TEXT("S3DInjector.dll")
#define  S3DOGLINJECTOR_DLL_NAME		TEXT("S3DOGLInjector.dll")
#endif 

#endif // DRIVER_NAME_H

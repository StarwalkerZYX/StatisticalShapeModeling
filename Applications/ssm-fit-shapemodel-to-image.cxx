﻿#include <boost/program_options.hpp>

#include <itkImageMomentsCalculator.h>
#include <itkTriangleMeshToBinaryImageFilter.h>
#include <itkStandardMeshRepresenter.h>

#include "ssmTypes.h"
#include "ssmUtils.h"
#include "ssmPointSetToImageMetrics.h"
#include "ssmInitializeSpatialTransform.h"
#include "ssmShapeModelToImageRegistrationMethod.h"

struct ProgramOptions
{
  bool help;
  std::string modelFile;
  std::string imageFile;
  std::string outputFile;
  std::string reportFile;
  std::string transformFile;
  size_t transform = 2;
  size_t iterations = 500;
  size_t degree = 2;
  double regularization = 0.1;
};

namespace po = boost::program_options;
po::options_description initializeProgramOptions(ProgramOptions& poParameters);

int main(int argc, char** argv)
{
  ProgramOptions options;
  po::options_description description = initializeProgramOptions(options);
  po::variables_map vm;
  try {
    po::parsed_options parsedOptions = po::command_line_parser(argc, argv).options(description).run();
    po::store(parsedOptions, vm);
    po::notify(vm);
  }
  catch (po::error& e) {
    cerr << "An exception occurred while parsing the command line:" << endl;
    cerr << e.what() << endl << endl;
    cout << description << endl;
    return EXIT_FAILURE;
  }
  if (options.help == true) {
    cout << description << endl;
    return EXIT_SUCCESS;
  }

  //----------------------------------------------------------------------------
  // read image
  BinaryImageType::Pointer image = BinaryImageType::New();
  if (!readImage<BinaryImageType>(image, options.imageFile)) {
    return EXIT_FAILURE;
  }
  std::cout << "image " << options.imageFile << std::endl;
  std::cout << "size    " << image->GetLargestPossibleRegion().GetSize() << std::endl;
  std::cout << "spacing " << image->GetSpacing() << std::endl;
  std::cout << std::endl;

  //----------------------------------------------------------------------------
  // read statistical shape model
  typedef itk::StandardMeshRepresenter<float, Dimension> RepresenterType;
  RepresenterType::Pointer representer = RepresenterType::New();

  typedef itk::StatisticalModel<MeshType> StatisticalModelType;
  StatisticalModelType::Pointer model = StatisticalModelType::New();
  try {
    model->Load(representer, options.modelFile.c_str());
  }
  catch (itk::ExceptionObject & excp) {
    std::cerr << excp << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << "model " << options.modelFile << std::endl;
  std::cout << "number of components " << model->GetNumberOfPrincipalComponents() << std::endl;
  std::cout << "number of points     " << model->GetRepresenter()->GetReference()->GetNumberOfPoints() << std::endl;
  std::cout << std::endl;

  //----------------------------------------------------------------------------
  // initialize spatial transform
  MeshType::BoundingBoxType::ConstPointer boundingBox = model->DrawMean()->GetBoundingBox();
  BinaryImageType::SpacingType spacing(1);
  BinaryImageType::PointType origin = boundingBox->GetMinimum();
  BinaryImageType::SizeType size;
  for (size_t n = 0; n < Dimension; ++n) {
    size[n] = (boundingBox->GetMaximum()[n] - boundingBox->GetMinimum()[n]) / spacing[n];
  }

  typedef itk::TriangleMeshToBinaryImageFilter<MeshType, BinaryImageType> ShapeToBinaryImageFilterType;
  ShapeToBinaryImageFilterType::Pointer shapeToImage = ShapeToBinaryImageFilterType::New();
  shapeToImage->SetInput(model->DrawMean());
  shapeToImage->SetSize(size);
  shapeToImage->SetOrigin(origin);
  shapeToImage->SetSpacing(spacing);
  shapeToImage->SetOutsideValue(0);
  shapeToImage->SetInsideValue(1);
  try {
    shapeToImage->Update();
  }
  catch (itk::ExceptionObject& excep) {
    std::cout << excep << std::endl;
    return EXIT_FAILURE;
  }

  // moment calculators
  typedef itk::ImageMomentsCalculator<BinaryImageType>  ImageCalculatorType;
  ImageCalculatorType::Pointer movingCalculator = ImageCalculatorType::New();
  movingCalculator->SetImage(shapeToImage->GetOutput());
  movingCalculator->Compute();

  ImageCalculatorType::Pointer fixedCalculator = ImageCalculatorType::New();
  fixedCalculator->SetImage(image);
  fixedCalculator->Compute();

  typedef ImageCalculatorType::VectorType VectorType;
  VectorType center = movingCalculator->GetCenterOfGravity();
  VectorType translation = fixedCalculator->GetCenterOfGravity() - movingCalculator->GetCenterOfGravity();

  // initialize spatial transform
  typedef ssm::InitializeSpatialTransform <double> InitializeSpatialTransformType;
  auto initializer = InitializeSpatialTransformType::New();
  initializer->SetCenter(center);
  initializer->SetTranslation(translation);
  initializer->SetTransformType(options.transform);
  try {
    initializer->Initialize();
  }
  catch (itk::ExceptionObject& excep) {
    std::cout << excep << std::endl;
    return EXIT_FAILURE;
  }
  initializer->PrintReport();

  //----------------------------------------------------------------------------
  // perform shape model to image registration
  typedef ssm::ShapeModelToImageRegistrationMethod<StatisticalModelType, BinaryImageType, MeshType> ShapeModelRegistrationMethodType;
  auto shapeModelToSurfaceRegistration = ShapeModelRegistrationMethodType::New();
  shapeModelToSurfaceRegistration->SetShapeModel(model);
  shapeModelToSurfaceRegistration->SetImage(image);
  shapeModelToSurfaceRegistration->SetComputeLevelSetImage(true);
  shapeModelToSurfaceRegistration->SetNumberOfIterations(options.iterations);
  shapeModelToSurfaceRegistration->SetSpatialTransform(initializer->GetTransform());
  shapeModelToSurfaceRegistration->SetSpatialScales(initializer->GetScales());
  shapeModelToSurfaceRegistration->GetMetric()->SetRegularizationParameter(options.regularization);
  shapeModelToSurfaceRegistration->GetMetric()->SetDegree(options.degree);
  shapeModelToSurfaceRegistration->SetModelScale(3);
  shapeModelToSurfaceRegistration->SetNumberOfEpochs(1);
  try {
    shapeModelToSurfaceRegistration->Update();
  }
  catch (itk::ExceptionObject& excep) {
    std::cerr << excep << std::endl;
    return EXIT_FAILURE;
  }
  shapeModelToSurfaceRegistration->PrintReport();

  // perform registration
  typedef std::pair<std::string, std::string> PairType;
  std::vector<PairType> info;
  info.push_back(PairType("Metric", std::to_string(shapeModelToSurfaceRegistration->GetOptimizer()->GetValue())));
  info.push_back(PairType("Elapsed time", std::to_string(shapeModelToSurfaceRegistration->GetElapsedTime())));

  // compute metrics
  typedef itk::PointSet<MeshType::PointType, MeshType::PointDimension> PointSetType;
  typedef ssm::PointSetToImageMetrics<PointSetType, ShapeModelRegistrationMethodType::LevelSetImageType> PointSetToImageMetricsType;
  PointSetToImageMetricsType::Pointer metrics = PointSetToImageMetricsType::New();
  metrics->SetPointSetAsMesh<MeshType>(shapeModelToSurfaceRegistration->GetOutput());
  metrics->SetImage(shapeModelToSurfaceRegistration->GetLevelSetImage());
  metrics->SetInfo(info);
  metrics->Compute();
  metrics->PrintReport();

  // write report to *.csv file
  std::cout << "print report to the file: " << options.reportFile << std::endl;
  metrics->PrintReportToFile(options.reportFile, getBaseNameFromPath(options.imageFile));

  // write surface
  std::cout << "write output surface to the file: " << options.outputFile << std::endl;
  if (!writeMesh<MeshType>(shapeModelToSurfaceRegistration->GetOutput(), options.outputFile)) {
    return EXIT_FAILURE;
  }

  // write transform
  if (options.transformFile != "") {
    std::cout << "write transform to the file: " << options.transformFile << std::endl;
    writeTransform(initializer->GetTransform(), options.transformFile);
  }

  return EXIT_SUCCESS;
}

po::options_description initializeProgramOptions(ProgramOptions& options)
{
  po::options_description mandatory("Mandatory options");
  mandatory.add_options()
    ("model,m", po::value<std::string>(&options.modelFile), "The path to the input shape model file.")
    ("image,i", po::value<std::string>(&options.imageFile), "The path to the input image file.")
    ("output,o", po::value<std::string>(&options.outputFile), "The path for the output surface file.")
    ;

  po::options_description input("Optional input options");
  input.add_options()
    ("transform", po::value<size_t>(&options.transform)->default_value(options.transform), "The type of the used spatial transform.")
    ("iterations", po::value<size_t>(&options.iterations)->default_value(options.iterations), "The number of iterations.")
    ("degree", po::value<size_t>(&options.degree)->default_value(options.degree), "The degree of residuals to compute shape model to image metric.")
    ("regularization", po::value<double>(&options.regularization)->default_value(options.regularization), "The regularization factor.")
    ;

  po::options_description output("Optional output options");
  output.add_options()
    ("output-transform", po::value<std::string>(&options.transformFile), "The path for the output transform file.")
    ;

  po::options_description report("Optional report options");
  report.add_options()
    ("report,r", po::value<std::string>(&options.reportFile), "The path for the file to print report.")
    ;

  po::options_description help("Optional options");
  help.add_options()
    ("help,h", po::bool_switch(&options.help), "Display this help message")
    ;

  po::options_description description;
  description.add(mandatory).add(input).add(output).add(report).add(help);

  return description;
}

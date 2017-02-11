%% IMREADSUB      Loads multiple image stacks into memory, applying motion correction and downsampling
function [movie, binnedMovie, inputSize, info] = imreadsub(imageFiles, motionCorr, frameGrouping, cropping, varargin)

  % Default arguments
  if nargin < 3 || isempty(frameGrouping)
    frameGrouping = 1;
  end
  if nargin < 4 || isempty(cropping)
    cropping      = [];
  end
  if ischar(imageFiles)
    imageFiles    = {imageFiles};
  end
  if ~isempty(varargin) && isequal(lower(varargin{1}), 'verbose')
    verbose       = true;
    varargin(1)   = [];
  else
    verbose       = false;
  end
  
  % Option to crop border containing motion correction artifacts (some frames with no data)
  if ~isempty(cropping)
    frameSize     = cropping.selectSize;
  elseif isempty(motionCorr)
    info          = ecs.imfinfox(imageFiles{1});
    frameSize     = [info.height, info.width];
  else
    frameSize     = size(motionCorr(1).reference);
  end

  % Option to store only a subset of pixels (creates a 2D pixel-by-time matrix)
  addGrouping     = [];
  if iscell(frameGrouping)
    pixelIndex    = frameGrouping{2};
    remainIndex   = frameGrouping{3};
    if numel(frameGrouping) > 3
      addGrouping = frameGrouping{4};
    end
    frameGrouping = frameGrouping{1};
  else
    pixelIndex    = [];
  end
  
  
  % Compute movie size
  info            = ecs.imfinfox(imageFiles);
  if isempty(motionCorr)
    dataType      = sprintf('%s%d', lower(info.sampleFormat), info.bitsPerSample);
  else
    dataType      = class(motionCorr(1).reference);
  end
  totalFrames     = ceil(sum(info.fileFrames) / frameGrouping);
  inputSize       = [frameSize, totalFrames];
  if isempty(pixelIndex)
    movieSize     = inputSize;
  else
    movieSize     = [numel(pixelIndex) + 1, totalFrames];
  end
  if verbose
    dataFcn       = str2func(dataType);
    movieBytes    = prod(movieSize) * numel(typecast(dataFcn(0), 'int8'));
    sizeStr       = arrayfun(@(x) sprintf('%d',x), movieSize, 'UniformOutput', false);
    fprintf ( '----  Allocating memory for %s pixels of type %s:  %s\n'             ...
            , strjoin(sizeStr, ' x '), dataType, formatSIPrefix(movieBytes, 'B')    ...
            );
  end
  
  
  % Preallocate output
  if numel(imageFiles) > 1 || ~isempty(pixelIndex)
    movie         = zeros(movieSize, dataType);
  end
  if isempty(addGrouping)
    binnedMovie   = [];
  else
    binnedMovie   = zeros([frameSize, ceil(sum(info.fileFrames) / frameGrouping / addGrouping)], dataType);
  end
  
  
  out             = struct('nFrames', {0}, 'nLeft', {0}, 'leftover', {[]});
  sub             = struct('nFrames', {0}, 'nLeft', {0}, 'leftover', {[]});
  for iFile = 1:numel(imageFiles)
    % Read in the image and apply motion correction shifts
    if isempty(motionCorr)
      img         = cv.imreadx(imageFiles{iFile}, [], [], varargin{:});
    elseif isfield(motionCorr(iFile), 'rigid')
      img         = ecs.imreadnonlin(imageFiles{iFile}, motionCorr(iFile));
    else
      img         = cv.imreadx(imageFiles{iFile}, motionCorr(iFile).xShifts(:,end), motionCorr(iFile).yShifts(:,end), varargin{:});
    end
    
    % Crop border if so requested
    if ~isempty(cropping)
      img         = rectangularSubset(img, cropping.selectMask, cropping.selectSize, 1);
    end
    
    % Rebin if so desired
    if ~isempty(frameGrouping) && frameGrouping > 1
      [img, range, out]       ...
                  = onTheFlyRebin(img, frameGrouping, out);
    else
      range       = out.nFrames + (1:size(img,3));
      out.nFrames = out.nFrames + size(img,3);
    end
  
    % Additional rebinning if specified
    if ~isempty(addGrouping)
      [binned, bRange, sub]   ...
                  = onTheFlyRebin(img, addGrouping, sub);
      binnedMovie(:,:,bRange)     = binned;
    end

    
    if ~isempty(pixelIndex)
      % Store only subset of pixels
      img         = reshape(img, [], size(img,3));
      movie(1:end-1,range)        = img(pixelIndex,:);
      movie(end    ,range)        = mean(img(remainIndex,:), 1);
      
    elseif numel(imageFiles) == 1
      % If there is only one input file, avoid allocation overhead
      movie                       = img;
      
    else
      % Full frame is stored if pixel indices are not specified
      movie(:,:,range)            = img;
    end
  end
  
  
  % Don't forget last frame in rebinned movies
  if out.nLeft > 0
    if isempty(pixelIndex)
      movie(:,:,out.nFrames+1)      = out.leftover / out.nLeft;
    else
      img         = reshape(out.leftover, [], 1);
      movie(1:end-1,out.nFrames+1)  = img(pixelIndex,:);
      movie(end    ,out.nFrames+1)  = mean(img(remainIndex,:), 1);
    end
  end
  if sub.nLeft > 0
    binnedMovie(:,:,sub.nFrames+1)  = sub.leftover / sub.nLeft;
  end
  
end


%%
function [binned, binRange, info] = onTheFlyRebin(img, binsPerGroup, info)
  
  % Preallocate output
  binned          = zeros(size(img,1), size(img,2), 0);
  
  % Get range of indices excluding those owed to a leftover bin
  if info.nLeft > 0
    index         = binsPerGroup - info.nLeft + 1:size(img,3);
    if ~isempty(index)
      binned      = (sum(img(:,:,1:index(1)-1), 3) + info.leftover) / (index(1) - 1 + info.nLeft);
    end
  else
    index         = 1:size(img,3);
  end

  % Collect rebinned movie and leftover frames
  if isempty(index)
    info.nLeft    = info.nLeft + size(img,3);
    info.leftover = info.leftover + sum(img,3);
  elseif index(end) - index(1) + 1 < binsPerGroup
    info.nLeft    = index(end) - index(1) + 1;
    info.leftover = sum(img, 3);
  else
    info.nLeft    = rem(index(end) - index(1) + 1, binsPerGroup);
    index         = index(1:numel(index) - info.nLeft);
    binned        = cat(3, binned, rebin(img(:,:,index), binsPerGroup, 3));
    info.leftover = sum(img(:,:,index(end)+1:end), 3);
  end
  
  % Output range in which to store binned movie
  binRange        = info.nFrames + (1:size(binned,3));
  info.nFrames    = info.nFrames + size(binned,3);
    
end

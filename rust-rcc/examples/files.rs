//! OS regression checks run inside disposable guests.
extern crate alloc;
use alloc::vec::Vec;
use rustcell::{
    Error, Result,
    fs::{self, File, OpenOptions, SeekFrom},
};

fn require(condition: bool) -> Result<()> {
    if condition {
        Ok(())
    } else {
        Err(Error::InvalidData)
    }
}

pub fn check() -> Result<()> {
    let args = rustcell::env::args()?;
    require(args.len() == 5)?;
    require(args[3].len() == 9 && args[3][3] == b' ' && args[3][8] == b's')?;
    require(args[4].is_empty())?;
    super::utility::uppercase(&args[1], &args[2])?;

    let path = b"/scratch";
    // Modes, byte offsets, short reads, EOF, and explicit close.
    let mut file = OpenOptions::new()
        .read(true)
        .write(true)
        .create_new(true)
        .open(path)?;
    file.write_all(b"abcdef")?;
    require(file.seek(SeekFrom::Start(2))? == 2)?;
    file.write_all(b"XY")?;
    require(file.seek(SeekFrom::Current(-2))? == 2)?;
    let mut pair = [0u8; 2];
    file.read_exact(&mut pair)?;
    require(pair[0] == b'X' && pair[1] == b'Y')?;
    require(file.seek(SeekFrom::End(-1))? == 5)?;
    require(file.read(&mut pair)? == 1 && pair[0] == b'f')?;
    require(file.read(&mut pair)? == 0)?;
    require(matches!(
        file.read_exact(&mut pair),
        Err(Error::UnexpectedEof)
    ))?;
    require(file.seek(SeekFrom::Start(u32::MAX)).is_err())?;
    file.close()?;
    require(
        OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(path)
            .is_err(),
    )?;
    OpenOptions::new()
        .append(true)
        .open(path)?
        .write_all(b"!")?;
    let bytes = fs::read(path)?;
    require(bytes.len() == 7 && bytes[2] == b'X' && bytes[6] == b'!')?;
    fs::write(path, b"z")?;
    require(fs::read(path)?.len() == 1)?;
    fs::remove_file(path)?;
    require(File::open(path).is_err())?;
    require(File::open(b"/bad\0name").is_err())?;
    require(File::open(b"").is_err())?;
    let long_path = [b'x'; 512];
    require(File::open(&long_path).is_err())?;
    require(OpenOptions::new().create(true).open(path).is_err())?;
    // A failed operation preserves its errno even after a successful operation.
    let error = match File::open(b"/missing") {
        Err(e) => e,
        Ok(_) => return Err(Error::InvalidData),
    };
    {
        let _file = File::open(&args[1])?;
    }
    require(matches!(error, Error::Os(2)))?;
    // More opens than the descriptor table can hold: Drop must release them.
    for _ in 0..80u16 {
        {
            let _file = File::open(&args[1])?;
        }
        require(matches!(early_eof(&args[1]), Err(Error::UnexpectedEof)))?;
    }
    // read_to_end appends, and short-file read_exact returns an error.
    let mut prefix = Vec::new();
    prefix.push(0x7f);
    let read = File::open(&args[1])?.read_to_end(&mut prefix)?;
    require(prefix.len() == read + 1 && prefix[0] == 0x7f)?;
    rustcell::io::write_stdout(b"RUSTCELL_FILES_OK\n")
}

fn early_eof(path: &[u8]) -> Result<()> {
    let mut file = File::open(path)?;
    file.seek(SeekFrom::End(0))?;
    file.read_exact(&mut [0u8; 1])?;
    Ok(())
}
